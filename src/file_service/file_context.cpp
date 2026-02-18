#include <mega/common/client.h>
#include <mega/common/database.h>
#include <mega/common/lock.h>
#include <mega/common/node_info.h>
#include <mega/common/partial_download.h>
#include <mega/common/scoped_query.h>
#include <mega/common/task_queue.h>
#include <mega/common/transaction.h>
#include <mega/common/upload.h>
#include <mega/common/utility.h>
#include <mega/file_service/displaced_buffer.h>
#include <mega/file_service/file_access.h>
#include <mega/file_service/file_append_request.h>
#include <mega/file_service/file_context.h>
#include <mega/file_service/file_context_badge.h>
#include <mega/file_service/file_fetch_request.h>
#include <mega/file_service/file_flush_request.h>
#include <mega/file_service/file_id.h>
#include <mega/file_service/file_info.h>
#include <mega/file_service/file_info_context.h>
#include <mega/file_service/file_location.h>
#include <mega/file_service/file_range_context.h>
#include <mega/file_service/file_read_request.h>
#include <mega/file_service/file_reclaim_request.h>
#include <mega/file_service/file_remove_request.h>
#include <mega/file_service/file_result.h>
#include <mega/file_service/file_service_context.h>
#include <mega/file_service/file_touch_request.h>
#include <mega/file_service/file_truncate_request.h>
#include <mega/file_service/file_write_request.h>
#include <mega/file_service/logging.h>
#include <mega/file_service/sparse_file_buffer.h>
#include <mega/file_service/type_traits.h>
#include <mega/filesystem.h>

#include <chrono>
#include <iterator>
#include <limits>
#include <type_traits>
#include <variant>

namespace mega
{
namespace file_service
{

using namespace common;

class FileContext::FetchContext
{
    // Called when the fetch has been completed.
    void completed(FileResult result);

    // Logs instance lifetime.
    common::InstanceLogger<FetchContext> mInstanceLogger;

    // Keep mContext alive as long as we are alive.
    Activity mActivity;

    // What file are we fetching?
    FileContext& mContext;

    // What fetch requests are we executing?
    std::vector<FileFetchRequest> mRequests;

public:
    FetchContext(FileContext& context, FileFetchRequest request);

    // Called when we've received file data.
    void operator()(FetchContextPtr& context, FileResultOr<FileReadResult> result);

    // Queue a fetch request for execution.
    void queue(FileFetchRequest request);
}; // FetchContext

class FileContext::FlushContext
{
    // Called when the file's content has been uploaded.
    void bound(FlushContextPtr& context, ErrorOr<NodeHandle> result);

    // Called when the flush has been completed.
    template<typename Lock>
    void completed(FlushContextPtr context, Lock&& lock, FileResult result);

    // Called to check that our upload target is valid.
    //
    // Populates mHandle, mName and mParentHandle.
    Error resolve(Client& client);

    // Called when the file's data has been uploaded.
    void uploaded(FlushContextPtr& context, ErrorOr<UploadResult> result);

    // Logs instance lifetime.
    common::InstanceLogger<FlushContext> mInstanceLogger;

    // Keep mContext alive as long as we are alive.
    Activity mActivity;

    // What file are we flushing?
    FileContext& mContext;

    // The file's current node handle.
    NodeHandle mHandle;

    // Where is this file stored in the cloud?
    FileLocation mLocation;

    // What flush requests are we executing?
    std::vector<FileFlushRequest> mRequests;

    // The upload that's pushing our content to the cloud.
    UploadPtr mUpload;

public:
    FlushContext(FileContext& context, FileFlushRequest request);

    // Called when we've retrieved all of this file's content.
    void operator()(FlushContextPtr& context, FileResult result);

    // Cancel the flush.
    template<typename Lock>
    static void cancel(FlushContextPtr context, Lock&& lock);

    // Queue a flush request for execution.
    void queue(FileFlushRequest request);
}; // FlushContext

class FileContext::ReclaimContext
{
    // Called when the reclaim request has completed.
    template<typename Lock>
    void completed(ReclaimContextPtr context, Lock&& lock, FileResultOr<std::uint64_t> result);

    // Logs instance lifetime.
    common::InstanceLogger<ReclaimContext> mInstanceLogger;

    // Keep mContext alive as long as we are alive.
    Activity mActivity;

    // How much space was the file taking when we started reclaiming?
    std::uint64_t mAllocatedSize;

    // Who should we call when the reclaim completes?
    std::vector<FileReclaimCallback> mCallbacks;

    // What file are we reclaiming?
    FileContext& mContext;

public:
    ReclaimContext(FileContext& context);

    // Cancel the reclamation.
    template<typename Lock>
    static void cancel(ReclaimContextPtr& context, Lock&& lock);

    // Called when the file's data has been flushed to the cloud.
    void flushed(ReclaimContextPtr& context, FileResult result);

    // Queue a callback for execution when the reclaim completes.
    void queue(FileReclaimCallback callback);
}; // ReclaimContext

// Retrieve an instance of a request's type tag.
template<typename Request>
auto tag(const Request& request)
    -> std::enable_if_t<IsFileRequestV<Request>, typename Request::Type>;

// Wrap a callback to ensure that exceptions are always handled.
template<typename Callback>
Callback swallow(Callback callback, const char* name);

void FileContext::addRange(const FileRange& range, Transaction& transaction)
{
    auto query = transaction.query(mService.queries().mAddFileRange);

    query.param(":begin").set(range.mBegin);
    query.param(":end").set(range.mEnd);
    query.param(":id").set(mInfo->id());

    query.execute();
}

void FileContext::cancel(const FileRange& range)
{
    // Make sure we have exclusive access to mDownloading.
    std::unique_lock lock(mDownloadingLock);

    // What ranges does range intersect?
    auto [begin, end] = mDownloading.find(range);

    // No ranges intersect range.
    if (begin == end)
        return;

    // Tracks any downloads in progress.
    std::vector<FileRangeContextPtr> contexts;

    // Tracks how many downloads are still in progress.
    std::atomic<std::size_t> count{0};

    // So we know when all downloads have completed.
    std::promise<void> notifier;

    // Called when a download has completed.
    auto completed = [&](auto)
    {
        // All downloads have completed.
        if (count.fetch_sub(1) == 1)
            notifier.set_value();
    }; // completed

    // Figure out which range downloads are in progress.
    for (; begin != end; ++begin)
    {
        // Convenience.
        auto context = begin->second;

        // Range doesn't have a download in progress.
        if (!context)
            continue;

        // Invoke our callback when the range's download completes.
        context->queue(completed);

        // Latch the context so we can cancel its download later.
        contexts.emplace_back(context);
    }

    // No range downloads need to be cancelled.
    if (contexts.empty())
        return;

    // Track how many downloads are in progress.
    count += contexts.size();

    // Release ranges lock.
    //
    // Any ranges that were waiting on the lock will now complete.
    //
    // NOTE: As this function is only called while processing a write
    // request, we can be assured that no ranges will be added after this
    // lock is released.
    lock.unlock();

    // Cancel any range downloads still in progress.
    for (auto& context: contexts)
        context->cancel();

    // Wait for range downloads to complete.
    notifier.get_future().get();
}

void FileContext::cancel(FileRequest& request)
{
    // Cancel the request.
    std::visit(
        [&](auto& request)
        {
            completed(std::move(request), FILE_CANCELLED);
        },
        request);
}

void FileContext::cancel()
{
    // When we execute this function, we know that no live references to
    // this instance can exist. We know this because this function is only
    // called from the instance's destructor.
    //
    // This doesn't mean that the instance is idle, however, as it is
    // possible that one or more downloads may still be in progress which
    // means that the client servicing those downloads may be executing
    // within us or about to execute within us.

    // Get a snapshot of our ranges.
    auto ranges = [&]()
    {
        std::lock_guard guard(mDownloadingLock);
        return mDownloading;
    }();

    // Cancel any downloads in progress.
    for (const auto& [_, context]: ranges)
    {
        if (context)
            context->cancel();
    }

    // Cancel the flush if necessary.
    {
        std::unique_lock lock(mFlushContextLock);

        if (mFlushContext)
            mFlushContext->cancel(mFlushContext, std::move(lock));
    }

    // Cancel reclamation if necessary.
    {
        std::unique_lock lock(mReclaimContextLock);

        if (mReclaimContext)
            mReclaimContext->cancel(mReclaimContext, std::move(lock));
    }

    // Latch the request queue.
    auto requests = [this]()
    {
        // Make sure no one else is messing with our request queue.
        std::lock_guard guard(mRequestsLock);

        // Latch the request queue.
        auto requests = std::exchange(mRequests, FileRequestList());

        // Make sure the queue's in a sane state.
        mRequests.clear();

        // Return queue to caller.
        return requests;
    }();

    // Cancel any pending requests.
    //
    // We know this won't cause any other requests to be queued as we know
    // there are no live references to this instance.
    while (!requests.empty())
    {
        // Cancel the request.
        cancel(requests.front());

        // Remove the request from our queue.
        requests.pop_front();
    }
}

void FileContext::completed(FileRangeMap<FileRangeContextPtr>::Iterator iterator, FileRange range)
try
{
    // Convenience.
    auto offset = range.mBegin;
    auto length = range.mEnd - offset;

    // No data for this range was downloaded.
    if (!length)
        return mDownloading.remove(iterator), void();

    // Figure out what ranges we can coalesce with.
    auto begin = [&]()
    {
        // We don't have a left neighbor.
        if (iterator == mDownloading.begin())
            return iterator;

        // Get an iterator to our left neighbor.
        auto candidate = std::prev(iterator);

        // Neighbor hasn't completed downloading.
        if (candidate->second)
            return iterator;

        // Neighbor isn't contiguous.
        if (candidate->first.mEnd != range.mBegin)
            return iterator;

        // Update range.
        range.mBegin = candidate->first.mBegin;

        // Return iterator to caller.
        return candidate;
    }();

    auto end = [&]()
    {
        // Get an iterator to our right neighbor.
        auto candidate = std::next(iterator);

        // We don't have a right neighbor.
        if (candidate == mDownloading.end())
            return candidate;

        // Neighbor hasn't completed downloading.
        if (candidate->second)
            return candidate;

        // Neighbor isn't contiguous.
        if (candidate->first.mBegin != range.mEnd)
            return candidate;

        // Update range.
        range.mEnd = candidate->first.mEnd;

        // Return iterator to caller.
        return std::next(candidate);
    }();

    // Mark range as present.
    iterator->second.reset();

    // Convenience.
    auto& database = mService.database();

    // Acquire database lock.
    UniqueLock databaseLock(database);

    // Start transaction so we can safely access the database.
    auto transaction = database.transaction();

    // Remove obsolete ranges from the database.
    removeRanges(range, transaction);

    // Add our new range to the database.
    addRange(range, transaction);

    // Update the file's size.
    updateSize(mInfo->size(), transaction);

    // Remove obsolete ranges from memory.
    mDownloading.remove(begin, end);

    // Add our new range to memory.
    mDownloading.add(range, nullptr);

    // Persist our changes.
    transaction.commit();
}

catch (std::runtime_error& exception)
{
    // Let debuggers know what went wrong.
    FSWarningF("Unable to complete file range download: %s: %s: %s",
               toString(mInfo->id()).c_str(),
               toString(range).c_str(),
               exception.what());

    // Consider the range absent.
    mDownloading.remove(iterator);
}

void FileContext::completed(BufferPtr buffer, FileReadRequest&& request)
{
    // Sanity.
    assert(buffer);

    // Convenience.
    auto [begin, end] = request.mRange;

    // Complete the user's request.
    completed(std::move(request), FileReadResult{*buffer, begin, end - begin}, std::move(buffer));
}

template<typename Request, typename Result, typename... Captures>
auto FileContext::completed(Request&& request, Result result, Captures&&... captures)
    -> std::enable_if_t<IsFileRequestV<Request>>
{
    // Sanity.
    assert(request.mCallback);

    // Make sure request has been passed by rvalue reference.
    static_assert(std::is_rvalue_reference_v<decltype(request)>);

    // Called to complete the user's request.
    auto wrapper = [=](auto& callback, auto& cookie, auto&, auto& tag, auto&&...) mutable
    {
        // Determine the callback's concrete type.
        using Callback = std::remove_reference_t<decltype(callback)>;

        // Pass result as is when possible otherwise pass it as an unexpected.
        if constexpr (std::is_invocable_v<Callback, Result>)
            std::exchange(callback, Callback())(result);
        else
            std::exchange(callback, Callback())(unexpected(result));

        // Check if our context is still alive.
        auto context = cookie.lock();

        // Context isn't alive.
        if (!context)
            return;

        // Let the context know the request has completed.
        executed(tag);

        // See if we can't execute any queued requests.
        context->execute();
    }; // wrapper

    // Queue the user's request for completion.
    mService.execute(std::bind(std::move(wrapper),
                               swallow(std::move(request.mCallback), request.name()),
                               weak_from_this(),
                               std::placeholders::_1,
                               tag(request),
                               std::forward<Captures>(captures)...));
}

void FileContext::completed(FileWriteRequest&& request)
{
    // Convenience.
    auto [begin, end] = request.mRange;

    // Complete the user's request.
    completed(std::move(request), FileWriteResult{begin, end - begin});
}

void FileContext::dequeued([[maybe_unused]] std::unique_lock<std::mutex> lock, FileReadRequestTag)
{
    assert(lock.mutex() == &mRequestsLock);
    assert(lock.owns_lock());
}

void FileContext::dequeued([[maybe_unused]] std::unique_lock<std::mutex> lock, FileWriteRequestTag)
{
    assert(lock.mutex() == &mRequestsLock);
    assert(lock.owns_lock());

    // Sanity.
    assert(mNumPendingWriteRequests);

    --mNumPendingWriteRequests;
}

void FileContext::dequeued(std::unique_lock<std::mutex> lock, const FileRequest& request)
{
    assert(lock.mutex() == &mRequestsLock);
    assert(lock.owns_lock());

    std::visit(
        [&lock, this](auto&& request)
        {
            this->dequeued(std::move(lock), tag(request));
        },
        request);
}

bool FileContext::executable(std::unique_lock<std::mutex>& lock,
                             bool queuing,
                             const FileRequest& request)
{
    assert(lock.mutex() == &mRequestsLock);
    assert(lock.owns_lock());

    return std::visit(
        [&lock, queuing, this](auto&& request)
        {
            return this->executable(lock, queuing, tag(request));
        },
        request);
}

bool FileContext::executable([[maybe_unused]] std::unique_lock<std::mutex>& lock,
                             bool queuing,
                             FileReadRequestTag)
{
    assert(lock.mutex() == &mRequestsLock);
    assert(lock.owns_lock());

    if (queuing && mNumPendingWriteRequests)
        return false;

    return mReadWriteState.read();
}

bool FileContext::executable([[maybe_unused]] std::unique_lock<std::mutex>& lock,
                             bool,
                             FileWriteRequestTag)
{
    assert(lock.mutex() == &mRequestsLock);
    assert(lock.owns_lock());

    return mReadWriteState.write();
}

void FileContext::execute(FileAppendRequest& request)
{
    // Convenience.
    auto size = mInfo->size();

    // Assume there's no range for us to grow.
    FileRange range(size, size + request.mLength);

    // Acquire ranges lock.
    std::unique_lock rangesLock(mDownloadingLock);

    // Assume we can grow the last range.
    auto candidate = mDownloading.rbegin();

    // Can grow the last range.
    if (!mDownloading.empty() && candidate->first.mEnd == size)
        range.mBegin = candidate->first.mBegin;
    else
        candidate = mDownloading.end();

    // Disambiguate.
    using file_service::write;

    // Try and write the user's data to disk.
    auto [length, _] = mBuffer->write(request.mBuffer, size, request.mLength);

    // Couldn't write all of the user's data to disk.
    if (length < request.mLength)
        return completed(std::move(request), FILE_FAILED);

    // Convenience.
    auto& database = mService.database();

    // Acquire database lock.
    UniqueLock databaseLock(database);

    // Start a transaction so we can safely modify the database.
    auto transaction = database.transaction();

    // Remove obsolete ranges from the database.
    removeRanges(range, transaction);

    // Add a new range to the database.
    addRange(range, transaction);

    // Compute the file's new modification time.
    auto modified = now();

    // Update the file's access and modification time.
    updateAccessAndModificationTimes(modified, modified, transaction);

    // Update the file's size.
    updateSize(range.mEnd, transaction);

    // Remove obsolete ranges from memory.
    mDownloading.remove(candidate, mDownloading.end());

    // Add new range to memory.
    mDownloading.add(range, nullptr);

    // Persist our changes.
    transaction.commit();

    // Tweak range.
    range.mBegin = size;

    // Update the file's attributes.
    mInfo->written(modified, range);

    // Queue the user's request for execution.
    completed(std::move(request), FILE_SUCCESS);
}

void FileContext::execute(FileFetchRequest& request)
{
    // Acquire fetch context lock.
    std::unique_lock lock(mFetchContextLock);

    // A fetch is already in progress.
    if (mFetchContext)
        return mFetchContext->queue(std::move(request));

    // Instantiate a context for our fetch.
    mFetchContext = std::make_shared<FetchContext>(*this, std::move(request));

    // Release flush context lock.
    lock.unlock();

    // Try and read all of the file's data.
    read(FileReadRequest{std::bind(&FetchContext::operator(),
                                   mFetchContext.get(),
                                   mFetchContext,
                                   std::placeholders::_1),
                         FileRange(0, mInfo->size())});
}

void FileContext::execute(FileFlushRequest request)
{
    // The file hasn't been modified.
    if (!mInfo->dirty())
        return completed(std::move(request), FILE_SUCCESS);

    // Acquire flush context lock.
    std::unique_lock lock(mFlushContextLock);

    // A flush is already in progress.
    if (mFlushContext)
        return mFlushContext->queue(std::move(request));

    // Instantiate a new flush context.
    mFlushContext = std::make_shared<FlushContext>(*this, std::move(request));

    // Unlock flush context lock.
    lock.unlock();

    // Fetch all of this file's data.
    fetch(FileFetchRequest{std::bind(&FlushContext::operator(),
                                     mFlushContext.get(),
                                     mFlushContext,
                                     std::placeholders::_1)});
}

void FileContext::execute(FileReadRequest& request)
{
    // The range the user wants to read.
    auto range = request.mRange;

    // The file's current size.
    auto size = mInfo->size();

    // Convenience.
    auto& [begin, end] = range;

    // Make sure the user's read doesn't extend beyond the end of the file.
    end = std::min(end, size);

    // Make sure the user's read doesn't end before it begins.
    end = std::max(begin, end);

    // Make sure the request's range has been clamped.
    request.mRange = range;

    // The user doesn't actually need to read anything.
    if (begin == end)
        return completed(mBuffer, std::move(request));

    // Update the file's access time.
    mInfo->accessed(now());

    // Make sure we have exclusive access to mDownloading.
    std::unique_lock lock(mDownloadingLock);

    // The read's contained within an existing range.
    if (auto i = mDownloading.endsAfter(begin); i != mDownloading.end() && i->first.mBegin <= begin)
    {
        // Range is still being downloaded.
        if (i->second)
            return i->second->queue(std::move(request));

        // Clamp the read so it stays within range.
        request.mRange.mEnd = std::min(i->first.mEnd, request.mRange.mEnd);

        // The range can completely or partially satisfy the read.
        return completed(displace(mBuffer, begin), std::move(request));
    }

    // Add a new range.
    auto [i, added] = mDownloading.tryAdd(
        [&]()
        {
            // We'd overlap an existing range.
            if (auto i = mDownloading.endsAfter(range.mBegin);
                i != mDownloading.end() && i->first.mBegin < range.mEnd)
                return FileRange(range.mBegin, i->first.mBegin);

            // Read doesn't overlap any existing range.
            return range;
        }(),
        nullptr);

    // Sanity.
    assert(added);

    // Instantiate a context so manage the range's download.
    i->second = std::make_shared<FileRangeContext>(mActivities.begin(), *this, i);

    // Queue the read on our context.
    i->second->queue(std::move(request));

    // Try and create a download for the range.
    auto download = i->second->download();

    // Couldn't create a download for range.
    if (!download)
        return;

    // Release range lock so we can begin the download.
    lock.unlock();

    // Begin the download.
    download->begin();
}

// When this request is executed, any pending downloads will have completed.
void FileContext::execute(FileReclaimRequest& request)
{
    // Make sure no one else is modifying mDownloading.
    std::lock_guard rangesLock(mDownloadingLock);

    // Convenience.
    auto& database = mService.database();

    // Acquire database lock.
    UniqueLock databaseLock(database);

    // So we can safely modify the database.
    auto transaction = database.transaction();

    // Represents the entire file.
    FileRange range(0, mInfo->size());

    // Remove all of this file's ranges from the database.
    removeRanges(range, transaction);

    // Couldn't reduce the file's size.
    if (!mBuffer->truncate(0))
        return completed(std::move(request), FILE_FAILED);

    // Remove all of the ranges from memory.
    mDownloading.clear();

    // Update the file's size.
    updateSize(mInfo->size(), transaction);

    // Persist our changes.
    transaction.commit();

    // How much space did we reclaim?
    auto reclaimed = request.mAllocatedSize - mInfo->allocatedSize();

    // Let waiters know how much space we reclaimed.
    completed(std::move(request), reclaimed);
}

void FileContext::execute(FileRemoveRequest& request)
{
    // File's already been removed.
    if (mInfo->removed())
        return completed(std::move(request), FILE_SUCCESS);

    // Cancel any pending downloads.
    cancel(FileRange(0, mInfo->size()));

    // Convenience.
    auto handle = mInfo->handle();
    auto replaced = request.mReplaced;
    auto serviceOnly = request.mServiceOnly;

    // We only need to remove the file from the service.
    if (handle.isUndef() || serviceOnly)
        return completed(std::move(request), setRemoved(replaced));

    // Called when the file's been removed.
    auto removed = [replaced, this](auto&&, auto&& request, auto result) mutable
    {
        // File was removed from the cloud.
        if (result == API_OK)
            return completed(std::move(request), setRemoved(replaced));

        // Couldn't remove the file from the cloud.
        completed(std::move(request), fileResultFromError(result));
    }; // removed

    // Ask the client to remove our file.
    mService.client().remove(std::bind(std::move(removed),
                                       mActivities.begin(),
                                       std::move(request),
                                       std::placeholders::_1),
                             handle);
}

void FileContext::execute(FileTouchRequest& request)
{
    // Compute the file's new access time.
    auto accessed = now();

    // Convenience.
    auto& database = mService.database();

    // Acquire database lock.
    UniqueLock databaseLock(database);

    // Start a transaction so we can safely modify the database.
    auto transaction = database.transaction();

    // Update the file's access and modification time.
    updateAccessAndModificationTimes(accessed, request.mModified, transaction);

    // Persist our changes.
    transaction.commit();

    // Update file attributes.
    mInfo->modified(accessed, request.mModified);

    // Queue the user's request for completion.
    completed(std::move(request), FILE_SUCCESS);
}

void FileContext::execute(FileTruncateRequest& request)
{
    // Convenience.
    auto newSize = request.mSize;
    auto oldSize = mInfo->size();

    // User isn't changing this file's size.
    if (newSize == oldSize)
        return completed(std::move(request), FILE_SUCCESS);

    // Grow or shrink the file as necessary.
    auto [databaseLock, transaction] =
        newSize > oldSize ? grow(newSize, oldSize) : shrink(newSize, oldSize);

    // Compute the file's new modification time.
    auto modified = now();

    // Update the file's access and modification times in the database.
    updateAccessAndModificationTimes(modified, modified, transaction);

    // Update the file's size in the database.
    updateSize(newSize, transaction);

    // Persist our changes.
    transaction.commit();

    // Update the file's attributes.
    mInfo->truncated(modified, newSize);

    // Queue the user's request to for completion.
    completed(std::move(request), FILE_SUCCESS);
}

void FileContext::execute(FileWriteRequest& request)
{
    // Convenience.
    auto& range = request.mRange;

    auto length = range.mEnd - range.mBegin;

    // Caller doesn't actually want to write anything.
    if (!length)
        return completed(std::move(request));

    // Caller hasn't passed us a valid buffer.
    if (!request.mBuffer)
        return completed(std::move(request), FILE_INVALID_ARGUMENTS);

    // Cancel any downloads in progress that intersect our write.
    cancel(range);

    // Get exclusive access to mDownloading.
    std::unique_lock rangesLock(mDownloadingLock);

    // Disambiguate.
    using file_service::write;

    // Try and write the caller's content to storage.
    std::tie(length, std::ignore) = mBuffer->write(request.mBuffer, range.mBegin, length);

    // Couldn't write any content to storage.
    if (!length)
        return completed(std::move(request), FILE_FAILED);

    // Compute actual end of the written range.
    range.mEnd = range.mBegin + length;

    // Convenience.
    using Iterator = decltype(mDownloading.begin());

    Iterator begin;
    Iterator end;

    // Compute initial effective range.
    FileRange effectiveRange = {std::min(mInfo->size(), range.mBegin), range.mEnd};

    // Find out which ranges we've touched.
    std::tie(begin, end) = mDownloading.find(extend(effectiveRange, 1));

    // Refine our effective range.
    effectiveRange = [&]()
    {
        // Assume range has no contiguous siblings.
        auto from = effectiveRange.mBegin;
        auto to = effectiveRange.mEnd;

        // Range has no siblings.
        if (begin == mDownloading.end())
            return FileRange(from, to);

        // Range has a sibling.
        from = std::min(begin->first.mBegin, from);
        to = std::max(begin->first.mEnd, to);

        // Range has a right sibling.
        if (end != mDownloading.end())
        {
            // Clarity.
            auto sibling = std::prev(end);

            // Recompute range's end point.
            to = std::max(sibling->first.mEnd, to);

            // Return effective range to caller.
            return FileRange(from, to);
        }

        // Range may have a right sibling.
        auto candidate = mDownloading.crbegin();

        // Range doesn't have a right sibling.
        if (candidate == mDownloading.crend())
            return FileRange(from, to);

        // Recompute range's end point.
        to = std::max(candidate->first.mEnd, to);

        // Return effective range to caller.
        return FileRange(from, to);
    }();

    // Convenience.
    auto& database = mService.database();

    // Acquire database lock.
    UniqueLock databaseLock(database);

    // Start a transaction so we can safely modify the database.
    auto transaction = database.transaction();

    // Remove obsolete ranges from the database.
    removeRanges(effectiveRange, transaction);

    // Add a new range to the database.
    addRange(effectiveRange, transaction);

    // Compute the file's new modification time.
    auto modified = now();

    // Update the file's access and modification times in the database.
    updateAccessAndModificationTimes(modified, modified, transaction);

    // Update the file's size in the database.
    updateSize(std::max(mInfo->size(), effectiveRange.mEnd), transaction);

    // Remove obsolete ranges from memory.
    mDownloading.remove(begin, end);

    // Add our new range to memory.
    mDownloading.add(effectiveRange, nullptr);

    // Persist our changes.
    transaction.commit();

    // Update the file's attributes.
    mInfo->written(modified, range);

    // Queue the user's request for completion.
    completed(std::move(request));
}

void FileContext::execute(FileRequest& request)
{
    // Executes a user's request.
    auto execute = [this](auto& request)
    {
        try
        {
            // Sanity.
            assert(request.mCallback);

            // Immediately reject the request if necessary.
            if (auto result = reject(request); result != FILE_SUCCESS)
                return completed(std::move(request), result);

            // Try and execute the request.
            this->execute(request);
        }
        catch (std::exception& exception)
        {
            // Threw an exception while executing request.
            FSErrorF("Unable to execute %s request: %s", request.name(), exception.what());

            // Try and fail the request.
            completed(std::move(request), FILE_FAILED);
        }
    }; // execute

    // Execute the user's request.
    std::visit(std::move(execute), request);
}

void FileContext::execute()
{
    // Execute as many requests as we can.
    while (true)
    {
        // Acquire lock.
        std::unique_lock lock(mRequestsLock);

        // There are no requests waiting to execute.
        if (mRequests.empty())
            return;

        // Request isn't executable.
        if (!executable(lock, false, mRequests.front()))
            return;

        // Pop the request off the queue.
        auto request = std::move(mRequests.front());

        mRequests.pop_front();

        // Perform post dequeue actions.
        dequeued(std::unique_lock(std::move(lock)), request);

        // Execute the request.
        execute(request);
    }
}

template<typename Request>
auto FileContext::executeOrQueue(Request&& request) -> std::enable_if_t<IsFileRequestV<Request>>
{
    // Sanity.
    assert(request.mCallback);

    // Make sure the request's been passed by rvalue reference.
    static_assert(std::is_rvalue_reference_v<decltype(request)>);

    // Request isn't executable so queue it for later execution.
    //
    // If executable(...) returns true, request will have acquired a read (or write) lock.
    if (std::unique_lock lock(mRequestsLock); !executable(lock, true, request))
        return queue(std::move(lock), std::forward<Request>(request));

    // Immediately reject the request if necessary.
    //
    // completed(...) needs to be called here as it expects a request to hold some lock.
    if (auto result = reject(request); result != FILE_SUCCESS)
        return completed(std::forward<Request>(request), result);

    // Otherwise execute the request.
    execute(request);
}

void FileContext::executed(FileReadRequestTag)
{
    mReadWriteState.readCompleted();
}

void FileContext::executed(FileWriteRequestTag)
{
    mReadWriteState.writeCompleted();
}

auto FileContext::grow(std::uint64_t newSize, std::uint64_t oldSize)
    -> std::pair<UniqueLock<Database>, Transaction>
{
    // Make sure we have exclusive access to mDownloading.
    std::lock_guard rangesLock(mDownloadingLock);

    // Convenience.
    auto& database = mService.database();

    // Acquire database lock.
    UniqueLock databaseLock(database);

    // So we can safely modify the database.
    auto transaction = database.transaction();

    // Get our hands on this file's last range.
    auto last = mDownloading.rbegin();

    // Assume the file has no range for us to enlarge.
    FileRange range(oldSize, newSize);

    // File has a range we can enlarge.
    if (last != mDownloading.rend() && last->first.mEnd == oldSize)
    {
        // Remove the range from the database.
        removeRanges(last->first, transaction);

        // Tweak our range.
        range.mBegin = last->first.mBegin;

        // Remove the range from memory.
        mDownloading.remove(last);
    }

    // (Re)?add the range to the database.
    addRange(range, transaction);

    // (Re)?add the range to memory.
    mDownloading.add(range, nullptr);

    // Return the transaction to our caller.
    return std::make_pair(std::move(databaseLock), std::move(transaction));
}

template<typename Request>
auto FileContext::queue(std::unique_lock<std::mutex> lock, Request&& request)
    -> std::enable_if_t<IsFileRequestV<Request>>
{
    assert(lock.mutex() == &mRequestsLock);
    assert(lock.owns_lock());

    // Convenience.
    using Type = std::remove_reference_t<Request>;
    using Tag = std::in_place_type_t<Type>;

    // Push all but reclaim requests onto the end of our queue.
    if constexpr (IsFileReclaimRequestV<Type>)
        mRequests.emplace_front(Tag(), std::forward<Request>(request));
    else
        mRequests.emplace_back(Tag(), std::forward<Request>(request));

    // Perform post-queue actions.
    queued(std::move(lock), tag(request));
}

void FileContext::queued([[maybe_unused]] std::unique_lock<std::mutex> lock, FileReadRequestTag)
{
    assert(lock.mutex() == &mRequestsLock);
    assert(lock.owns_lock());
}

void FileContext::queued([[maybe_unused]] std::unique_lock<std::mutex> lock, FileWriteRequestTag)
{
    assert(lock.mutex() == &mRequestsLock);
    assert(lock.owns_lock());

    ++mNumPendingWriteRequests;
}

template<typename Request>
auto FileContext::reject([[maybe_unused]] const Request& request)
    -> std::enable_if_t<IsFileRequestV<Request>, FileResult>
{
    if constexpr (!IsFileReclaimRequestV<Request> && IsFileWriteRequestV<Request>)
    {
        if constexpr (IsFileRemoveRequestV<Request>)
        {
            if (request.mServiceOnly)
                return FILE_SUCCESS;
        }

        if (mKeyData)
            return FILE_READONLY;
    }

    return FILE_SUCCESS;
}

void FileContext::removeRanges(const FileRange& range, Transaction& transaction)
{
    auto query = transaction.query(mService.queries().mRemoveFileRanges);

    query.param(":begin").set(range.mBegin);
    query.param(":end").set(range.mEnd);
    query.param(":id").set(mInfo->id());

    query.execute();
}

FileResult FileContext::setRemoved(bool replaced)
try
{
    // Convenience.
    auto& database = mService.database();
    auto& queries = mService.queries();

    // Acquire database lock.
    std::lock_guard lock(database);

    // Mark the file as removed in the database.
    auto transaction = database.transaction();
    auto query = transaction.query(queries.mSetFileRemoved);

    query.param(":id").set(mInfo->id());
    query.execute();

    // Persist our changes.
    transaction.commit();

    // Mark the file as removed in memory.
    mInfo->removed(replaced);

    // Let the caller know the file was removed.
    return FILE_SUCCESS;
}

catch (std::runtime_error& exception)
{
    // Let debuggers know why we couldn't remove the file.
    FSErrorF("Unable to mark file %s as removed: %s",
             toString(mInfo->id()).c_str(),
             exception.what());

    // Let our caller know we couldn't remove the file.
    return FILE_FAILED;
}

auto FileContext::shrink(std::uint64_t newSize, std::uint64_t oldSize)
    -> std::pair<UniqueLock<Database>, Transaction>
{
    // Cancel any downloads in progress that would be "cut off."
    cancel(FileRange(oldSize, newSize));

    // So we have exclusive access to mDownloading.
    std::lock_guard rangesLock(mDownloadingLock);

    // Convenience.
    auto& database = mService.database();

    // Acquire database lock.
    UniqueLock databaseLock(database);

    // So we can safely modify the database.
    auto transaction = database.transaction();

    // Couldn't reduce the file's size.
    if (!mBuffer->truncate(newSize))
        throw FSError1("Couldn't reduce file size");

    // What ranges end at or after our file's new size?
    auto begin = mDownloading.endsAtOrAfter(newSize);

    // No ranges end at or after our new size.
    if (begin == mDownloading.end())
        return std::make_pair(std::move(databaseLock), std::move(transaction));

    // Convenience.
    FileRange range(begin->first.mBegin, oldSize);

    // Remove affected ranges from the database.
    removeRanges(range, transaction);

    // Remove affected ranges from memory.
    mDownloading.remove(begin, mDownloading.end());

    // First range has been "cut" by the file's new size.
    if (range.mBegin < newSize)
    {
        // Adjust the range's end point.
        range.mEnd = newSize;

        // Readd the range to the database.
        addRange(range, transaction);

        // Readd the range to memory.
        mDownloading.add(range, nullptr);
    }

    // Return the transaction to our caller.
    return std::make_pair(std::move(databaseLock), std::move(transaction));
}

void FileContext::updateAccessAndModificationTimes(std::int64_t accessed,
                                                   std::int64_t modified,
                                                   Transaction& transaction)
{
    auto query = transaction.query(mService.queries().mSetFileModificationTime);

    query.param(":accessed").set(accessed);
    query.param(":modified").set(modified);
    query.param(":id").set(mInfo->id());

    query.execute();
}

void FileContext::updateSize(std::uint64_t size, Transaction& transaction)
{
    auto query = transaction.query(mService.queries().mSetFileSize);

    query.param(":allocated_size").set(mInfo->allocatedSize());
    query.param(":id").set(mInfo->id());
    query.param(":reported_size").set(mInfo->reportedSize());
    query.param(":size").set(size);

    query.execute();
}

FileContext::FileContext(Activity activity,
                         FileAccessPtr file,
                         FileInfoContextPtr info,
                         std::optional<NodeKeyData> keyData,
                         FileRangeSet ranges,
                         FileServiceContext& service):
    enable_shared_from_this(),
    mInstanceLogger("FileContext", *this, logger()),
    mActivity(std::move(activity)),
    mBuffer(std::make_shared<SparseFileBuffer>(*file, *info)),
    mDownloading(),
    mDownloadingLock(),
    mInfo(std::move(info)),
    mFetchContext(),
    mFetchContextLock(),
    mFile(std::move(file)),
    mFlushContext(),
    mFlushContextLock(),
    mKeyData(std::move(keyData)),
    mNumPendingWriteRequests(0u),
    mOnDisk(std::move(ranges)),
    mOnDiskLock(),
    mReadWriteState(),
    mReclaimContext(),
    mReclaimContextLock(),
    mRequests(),
    mRequestsLock(),
    mService(service),
    mActivities()
{
    for (auto& range: mOnDisk)
        mDownloading.add(range, nullptr);
}

FileContext::~FileContext()
{
    // Cancel any downloads or pending requests.
    cancel();

    // Remove ourselves from our service's index.
    mService.removeFromIndex(FileContextBadge(), mInfo->id());
}

FileEventObserverID FileContext::addObserver(FileEventObserver observer)
{
    return mInfo->addObserver(std::move(observer));
}

void FileContext::append(FileAppendRequest request)
{
    executeOrQueue(std::move(request));
}

AutoFileHandle FileContext::dupFileDescriptor()
{
    assert(mFile);

    return mFile->dupFileDescriptor();
}

void FileContext::fetch(FileFetchRequest request)
{
    executeOrQueue(std::move(request));
}

void FileContext::fetchBarrier(FileFetchBarrierCallback callback)
{
    // Sanity.
    assert(callback);

    // Acquire range lock.
    std::unique_lock lock(mDownloadingLock);

    // True if a download is in progress.
    auto fetching = [](const auto& entry)
    {
        return entry.second != nullptr;
    }; // fetching

    // How many fetches are in progress?
    auto count = std::count_if(mDownloading.begin(), mDownloading.end(), fetching);

    // No fetches are in progress.
    if (!count)
    {
        // Release range lock.
        lock.unlock();

        // Invoke user callback.
        return callback();
    }

    // To avoid copying state needlessly.
    struct BarrierContext
    {
        BarrierContext(FileFetchBarrierCallback callback, std::size_t count):
            mCallback(std::move(callback)),
            mCount{count}
        {}

        FileFetchBarrierCallback mCallback;
        std::atomic<std::size_t> mCount;
    }; // BarrierContext

    // Instantiate barrier context.
    auto context = std::make_shared<BarrierContext>(std::move(callback), count);

    // Called when a fetch has completed.
    auto fetched = [context](FileResult)
    {
        // Invoke the user's callback when all fetches have completed.
        if (context->mCount.fetch_sub(1) == 1)
            context->mCallback();
    }; // fetched

    // Make sure fetched is called when each fetch has completed.
    for (const auto& entry: mDownloading)
    {
        if (entry.second)
            entry.second->queue(fetched);
    }
}

void FileContext::flush(FileFlushRequest request)
{
    executeOrQueue(std::move(request));
}

FileInfo FileContext::info() const
{
    return FileInfo(FileContextBadge(), mInfo);
}

FileRangeVector FileContext::ranges() const
{
    // Will store the ranges we'll return our caller.
    FileRangeVector ranges;

    // Get exclusive access to mDownloading.
    std::lock_guard guard(mDownloadingLock);

    // Populate our range vector.
    std::transform(mDownloading.begin(),
                   mDownloading.end(),
                   std::back_inserter(ranges),
                   SelectFirst());

    // Return ranges to our caller.
    return ranges;
}

void FileContext::read(FileReadRequest request)
{
    executeOrQueue(std::move(request));
}

void FileContext::reclaim(FileReclaimCallback callback)
{
    // Make sure we have exclusive access to mReclaimContext.
    std::lock_guard lock(mReclaimContextLock);

    // A reclaim request is already in progress.
    if (mReclaimContext)
        return mReclaimContext->queue(std::move(callback));

    // Create a new reclaim context.
    mReclaimContext = std::make_shared<ReclaimContext>(*this);

    // Queue our callback for later execution.
    mReclaimContext->queue(std::move(callback));

    // So we can use the context's flushed method as a callback.
    FileFlushCallback flushed = std::bind(&ReclaimContext::flushed,
                                          mReclaimContext.get(),
                                          mReclaimContext,
                                          std::placeholders::_1);

    // Make sure this file's data has been flushed to the cloud.
    flush(FileFlushRequest{std::move(flushed)});
}

void FileContext::remove(FileRemoveRequest request)
{
    executeOrQueue(std::move(request));
}

void FileContext::removeObserver(FileEventObserverID id)
{
    mInfo->removeObserver(id);
}

bool FileContext::removed() const
{
    return mInfo->removed();
}

void FileContext::touch(FileTouchRequest request)
{
    executeOrQueue(std::move(request));
}

void FileContext::truncate(FileTruncateRequest request)
{
    executeOrQueue(std::move(request));
}

void FileContext::write(FileWriteRequest request)
{
    executeOrQueue(std::move(request));
}

void FileContext::FetchContext::completed(FileResult result)
{
    // Acquire fetch context lock.
    std::unique_lock lock(mContext.mFetchContextLock);

    // Clear fetch context.
    mContext.mFetchContext = nullptr;

    // Steal queued requests.
    auto requests = std::exchange(mRequests, decltype(mRequests)());

    // Release fetch context lock.
    lock.unlock();

    // Execute queued requests.
    for (auto& request: requests)
        mContext.completed(std::move(request), result);
}

FileContext::FetchContext::FetchContext(FileContext& context, FileFetchRequest request):
    mInstanceLogger("FetchContext", *this, logger()),
    mActivity(context.mActivities.begin()),
    mContext(context),
    mRequests()
{
    // Queue the request.
    queue(std::move(request));
}

void FileContext::FetchContext::operator()(FetchContextPtr& context,
                                           FileResultOr<FileReadResult> result)
{
    // Couldn't read this file's data.
    if (!result)
        return completed(result.error());

    // No more content to read.
    if (!result->mLength)
        return completed(FILE_SUCCESS);

    // Convenience.
    auto offset = result->mOffset + result->mLength;
    auto length = mContext.mInfo->size() - offset;

    // Try and read the rest of the file's data.
    mContext.read(FileReadRequest{
        std::bind(&FetchContext::operator(), this, std::move(context), std::placeholders::_1),
        FileRange(offset, offset + length)});
}

void FileContext::FetchContext::queue(FileFetchRequest request)
{
    // Acquire fetch context lock.
    std::lock_guard guard(mContext.mFetchContextLock);

    // Queue the request.
    mRequests.emplace_back(std::move(request));
}

void FileContext::FlushContext::bound(FlushContextPtr& context, ErrorOr<NodeHandle> result)
{
    // Acquire flush context lock.
    std::unique_lock lock(mContext.mFlushContextLock);

    // Couldn't flush the file's content.
    if (!result)
        return completed(std::move(context), std::move(lock), fileResultFromError(result.error()));

    // Try and update the file's handle.
    try
    {
        // Convenience.
        auto& info = *mContext.mInfo;
        auto& service = mContext.mService;
        auto& database = service.database();

        // Acquire database lock.
        UniqueLock databaseLock(database);

        // Try and update the file's handle.
        auto transaction = database.transaction();
        auto query = transaction.query(service.queries().mSetFileHandle);

        query.param(":handle").set(*result);
        query.param(":id").set(info.id());

        query.execute();

        // Persist our changes.
        transaction.commit();

        // Update the file's node handle.
        info.flushed(*result);

        // File's flushed.
        completed(std::move(context), std::move(lock), FILE_SUCCESS);
    }
    catch (std::exception& exception)
    {
        // Let debuggers know why the flush failed.
        FSErrorF("Couldn't update file handle: %s: %s",
                 toString(mContext.mInfo->id()).c_str(),
                 exception.what());

        // Couldn't flush the file's content.
        completed(std::move(context), std::move(lock), FILE_FAILED);
    }
}

template<typename Lock>
void FileContext::FlushContext::completed(FlushContextPtr context, Lock&& lock, FileResult result)
{
    // Sanity.
    assert(lock.mutex() == &mContext.mFlushContextLock);
    assert(lock.owns_lock());

    // Make sure we're still the file's current flush context.
    if (mContext.mFlushContext == context)
    {
        // We are. Let our file know this flush has completed.
        mContext.mFlushContext = nullptr;
    }

    // Steal queued requests.
    auto requests = std::exchange(mRequests, decltype(mRequests)());

    // Release lock.
    lock.unlock();

    // Execute queued requests.
    for (auto& request: requests)
        mContext.completed(std::move(request), result);
}

Error FileContext::FlushContext::resolve(Client& client)
{
    // File's never been flushed before.
    if (mHandle.isUndef())
        return client.get(mLocation.mParentHandle).errorOr(API_OK);

    // Check if the file's node still exists.
    auto node = client.get(mHandle);

    // File's node no longer exists.
    if (!node)
        return node.error();

    // Latch the node's current name and parent.
    mLocation.mName = std::move(node->mName);
    mLocation.mParentHandle = node->mParentHandle;

    // Let the caller know the node still exists.
    return API_OK;
}

void FileContext::FlushContext::uploaded(FlushContextPtr& context, ErrorOr<UploadResult> result)
{
    // Acquire flush context lock.
    std::unique_lock lock(mContext.mFlushContextLock);

    // Couldn't upload the file's data.
    if (!result)
        return completed(std::move(context), std::move(lock), fileResultFromError(result.error()));

    // The file's been removed.
    if (mContext.mInfo->removed())
        return completed(std::move(context), std::move(lock), FILE_REMOVED);

    // Upload's been cancelled.
    if (mRequests.empty())
        return;

    // Release the lock.
    lock.unlock();

    // So we can use our bound method as a callback.
    BoundCallback bound =
        std::bind(&FlushContext::bound, this, std::move(context), std::placeholders::_1);

    // Bind a name to our file's uploaded data.
    (*result)(std::move(bound), mHandle);
}

FileContext::FlushContext::FlushContext(FileContext& context, FileFlushRequest request):
    mInstanceLogger("FlushContext", *this, logger()),
    mActivity(context.mActivities.begin()),
    mContext(context),
    mHandle(context.mInfo->handle()),
    mLocation(context.mInfo->location().value()),
    mRequests(),
    mUpload()
{
    mRequests.emplace_back(std::move(request));
}

void FileContext::FlushContext::operator()(FlushContextPtr& context, FileResult result)
{
    // Convenience.
    auto& mutex = mContext.mFlushContextLock;

    // Couldn't retrieve this file's content.
    if (result != FILE_SUCCESS)
        return completed(std::move(context), std::unique_lock(mutex), result);

    // Convenience.
    auto& service = mContext.mService;
    auto& client = service.client();
    auto& info = *mContext.mInfo;

    // Check whether the file or its intended parent still exists.
    result = fileResultFromError(resolve(client));

    // Acquire context lock.
    std::unique_lock lock(mutex);

    // File or its intended parent no longer exists.
    if (result != FILE_SUCCESS)
        return completed(std::move(context), std::move(lock), result);

    // No requests? Flush must have been cancelled.
    if (mRequests.empty())
        return;

    // Where is this file's data stored?
    auto path = service.path(info.id());

    // Convenience.
    auto& [name, parent] = mLocation;

    // Instantiate an upload.
    mUpload = client.upload(path, name, parent, path);

    // So we can use our uploaded method as a callback.
    UploadCallback callback =
        std::bind(&FlushContext::uploaded, this, std::move(context), std::placeholders::_1);

    // Begin the upload.
    mUpload->begin(std::move(callback));
}

template<typename Lock>
void FileContext::FlushContext::cancel(FlushContextPtr context, Lock&& lock)
{
    assert(context);
    assert(lock.mutex() == &context->mContext.mFlushContextLock);
    assert(lock.owns_lock());

    auto upload = std::exchange(context->mUpload, nullptr);

    // No upload's in progress.
    if (!upload)
        return context->completed(context, std::move(lock), FILE_CANCELLED);

    // Release the lock.
    lock.unlock();

    // Cancel the upload.
    upload->cancel();
}

void FileContext::FlushContext::queue(FileFlushRequest request)
{
    // Acquire flush context lock.
    std::lock_guard guard(mContext.mFlushContextLock);

    // Queue the request.
    mRequests.emplace_back(std::move(request));
}

template<typename Lock>
void FileContext::ReclaimContext::completed(ReclaimContextPtr context,
                                            Lock&& lock,
                                            FileResultOr<std::uint64_t> result)
{
    // Sanity.
    assert(lock.mutex() == &mContext.mReclaimContextLock);
    assert(lock.owns_lock());

    // Make sure we're still the current reclaim context.
    if (mContext.mReclaimContext == context)
    {
        // We are so let the file know we've completed.
        mContext.mReclaimContext = nullptr;
    }

    // Steal queued callbacks.
    auto callbacks = std::exchange(mCallbacks, decltype(mCallbacks)());

    // Release reclaim context lock.
    lock.unlock();

    // Convenience.
    auto& service = mContext.mService;

    // Execute queued callbacks.
    for (auto& callback: callbacks)
        service.execute(std::bind(std::move(callback), result));
}

FileContext::ReclaimContext::ReclaimContext(FileContext& context):
    mInstanceLogger("ReclaimContext", *this, logger()),
    mActivity(context.mActivities.begin()),
    mAllocatedSize(context.mInfo->allocatedSize()),
    mCallbacks(),
    mContext(context)
{}

template<typename Lock>
void FileContext::ReclaimContext::cancel(ReclaimContextPtr& context, Lock&& lock)
{
    // Sanity.
    assert(context);
    assert(lock.mutex() == &context->mContext.mReclaimContextLock);
    assert(lock.owns_lock());

    context->completed(context, std::forward<Lock>(lock), FILE_CANCELLED);
}

void FileContext::ReclaimContext::flushed(ReclaimContextPtr& context, FileResult result)
{
    // Acquire reclaim context lock.
    std::unique_lock lock(mContext.mReclaimContextLock);

    // Couldn't flush this file's data to the cloud.
    if (result != FILE_SUCCESS)
        return completed(std::move(context), std::move(lock), result);

    // Reclamation has been cancelled.
    if (mCallbacks.empty())
        return;

    // Release reclaim context lock.
    lock.unlock();

    // So we can use this context's completed method as a callback.
    auto callback = [context = std::move(context), this](auto result) mutable
    {
        completed(std::move(context), std::unique_lock(mContext.mReclaimContextLock), result);
    }; // callback

    // Queue the reclaim request for execution.
    mContext.queue(std::unique_lock(mContext.mRequestsLock),
                   FileReclaimRequest{mAllocatedSize, std::move(callback)});
}

void FileContext::ReclaimContext::queue(FileReclaimCallback callback)
{
    // Sanity.
    assert(callback);

    // Queue the callback for later execution.
    mCallbacks.emplace_back(swallow(std::move(callback), "reclaim"));
}

template<typename Callback>
Callback swallow(Callback callback, const char* name)
{
    return [callback = std::move(callback), name](auto&&... arguments)
    {
        try
        {
            // Try and execute the user's callback.
            std::invoke(callback, std::forward<decltype(arguments)>(arguments)...);
        }
        catch (std::exception& exception)
        {
            // User's callback threw an exception we can log.
            FSErrorF("User %s callback threw an exception: %s", name, exception.what());
        }
    };
}

template<typename Request>
auto tag(const Request&) -> std::enable_if_t<IsFileRequestV<Request>, typename Request::Type>
{
    return typename Request::Type();
}

} // file_service
} // mega
