#include <mega/common/client.h>
#include <mega/common/database.h>
#include <mega/common/expected.h>
#include <mega/common/partial_download.h>
#include <mega/common/scoped_query.h>
#include <mega/common/transaction.h>
#include <mega/file_service/buffer.h>
#include <mega/file_service/displaced_buffer.h>
#include <mega/file_service/file_buffer.h>
#include <mega/file_service/file_context.h>
#include <mega/file_service/file_info_context.h>
#include <mega/file_service/file_range.h>
#include <mega/file_service/file_range_context.h>
#include <mega/file_service/file_read_request.h>
#include <mega/file_service/file_result.h>
#include <mega/file_service/file_service_context.h>
#include <mega/file_service/file_service_options.h>
#include <mega/file_service/memory_buffer.h>
#include <mega/types.h>

#include <cassert>

namespace mega
{
namespace file_service
{

using namespace common;

// Check if result is a retryable error.
static bool retryable(const Error& result);

void FileRangeContext::completed(Error error)
{
    // Get a reference to our context.
    //
    // We're doing this here for two reasons:
    //
    // 1. We want to make sure this instance is kept alive until we've
    //    finished processing this donwload's completion.
    //
    // 2. We want to make sure that the lock we acquire immediately below is
    //    released before this instance itself is destroyed.
    [[maybe_unused]] auto context = std::move(mIterator->second);

    // Acquire lock on our file's map of downloading ranges.
    std::unique_lock lock(mContext.mDownloadingLock);

    // Remove ourselves from our file's map of downloading ranges.
    mContext.mDownloading.remove(mIterator);

    // Translate SDK error code to a file result.
    auto result = fileResultFromError(error);

    // Download didn't complete successfully.
    if (result != FILE_SUCCESS)
    {
        // Fail any remaining requests.
        for (auto i = mRequests.begin(); i != mRequests.end();)
        {
            // Convenience.
            auto& request = const_cast<FileReadRequest&>(*i);

            // Fail the request.
            mContext.completed(std::move(request), result);

            // Remove the request from our set.
            i = mRequests.erase(i);
        }
    }

    // Let any waiters know this range's download has completed.
    for (auto& callback: mCallbacks)
        mContext.mService.execute(std::bind(std::move(callback), result));
}

auto FileRangeContext::data(const void* buffer,
                            std::uint64_t offset,
                            std::uint64_t length,
                            const Speeds&) -> std::variant<Abort, Continue>
{
    // Records how much data we could write to disk.
    std::uint64_t count;

    // Try and write data to disk.
    try
    {
        // Acquire on disk map lock.
        std::lock_guard onDiskLock(mContext.mOnDiskLock);

        // Try and write data to our buffer.
        std::tie(count, std::ignore) = mContext.mBuffer->write(buffer, offset, length);

        // Couldn't write any data to disk.
        if (!count)
            return Abort();

        // Convenience.
        auto& onDisk = mContext.mOnDisk;

        // The range we've written.
        FileRange range(offset, offset + count);

        // Is this write appending data to a range on our left?
        auto left = onDisk.endsAt(offset);

        // Write is appending data to the range on our left.
        if (left != onDisk.end())
            range.mBegin = left->mBegin;

        // Is this write prepending data to a range on our right?
        auto right = onDisk.beginsAt(range.mEnd);

        // Write is prepending data to the right on our right.
        if (right != onDisk.end())
            range.mEnd = right->mEnd;

        // Convenience.
        auto& database = mContext.mService.database();

        // Acquire database lock.
        std::lock_guard databaseLock(database);

        // Begin a transaction so we can safely modify the database.
        auto transaction = database.transaction();

        // Remove obsolete ranges from the database.
        mContext.removeRanges(range, transaction);

        // Add our new range to the database.
        mContext.addRange(range, transaction);

        // Update the file's size.
        mContext.updateSize(mContext.mInfo->size(), transaction);

        // Persist database changes.
        transaction.commit();

        // Remove obsolete ranges from memory.
        if (left != onDisk.end())
            onDisk.remove(left);

        if (right != onDisk.end())
            onDisk.remove(right);

        // Add the new range in memory.
        onDisk.add(range);
    }
    catch (std::runtime_error&)
    {
        return Abort();
    }

    // Acquire downloading map lock.
    std::unique_lock downloadingLock(mContext.mDownloadingLock);

    // Bump our end position.
    mEnd += count;

    // Dispatch any requests that can now be satisfied.
    dispatch();

    // Couldn't write all of the data to disk.
    if (count < length)
        return Abort();

    // Continue the download.
    return Continue();
}

void FileRangeContext::dispatch()
{
    // What requests might we be able to satisfy?
    auto i = mRequests.begin();
    auto j = mRequests.lower_bound(mEnd);

    // Dispatch as many requests as we can.
    while (i != j)
    {
        // Copying the iterator keeps logic clean.
        auto k = i++;

        // Evil but necessary.
        auto& request = const_cast<FileReadRequest&>(*k);

        // Request has been dispatched.
        if (dispatch(request))
            mRequests.erase(k);
    }
}

bool FileRangeContext::dispatch(FileReadRequest& request)
{
    // Convenience.
    auto& range = request.mRange;

    // Can't dispatch this request.
    if (range.mBegin >= mEnd)
        return false;

    // Clamp the range as necessary.
    range.mEnd = std::min(mEnd, range.mEnd);

    // Dispatch the request.
    mContext.completed(displace(mContext.mBuffer, range.mBegin), std::move(request));

    // Let the caller know the request was dispatched.
    return true;
}

auto FileRangeContext::failed(Error result, int retries) -> std::variant<Abort, Retry>
{
    // Failure isn't due to a retryable error.
    if (!retryable(result))
        return Abort();

    // Convenience.
    auto options = mContext.mService.options();

    // Or if we've already retried the download too many times.
    if (static_cast<std::uint64_t>(retries) >= options.mMaximumRangeRetries)
        return Abort();

    // Retry the download.
    return options.mRangeRetryBackoff;
}

FileRangeContext::FileRangeContext(Activity activity,
                                   FileContext& context,
                                   FileRangeMap<FileRangeContextPtr>::Iterator iterator):
    PartialDownloadCallback(),
    mInstanceLogger("FileRangeContext", *this, logger()),
    mActivity(std::move(activity)),
    mCallbacks(),
    mContext(context),
    mDownload(),
    mEnd(iterator->first.mBegin),
    mIterator(iterator),
    mRequests()
{}

FileRangeContext::~FileRangeContext()
{
    // No requests should be queued at this point.
    assert(mRequests.empty());
}

void FileRangeContext::cancel()
{
    // Download's alive so cancel it.
    if (auto download = mDownload)
        download->cancel();
}

auto FileRangeContext::download() -> PartialDownloadPtr
{
    // Sanity.
    assert(!mDownload);

    // Convenience.
    auto& client = mContext.mService.client();
    auto& keyData = mContext.mKeyData;

    auto handle = mContext.mInfo->handle();
    auto offset = mIterator->first.mBegin;
    auto length = mIterator->first.mEnd - offset;

    // Try and create a partial download.
    auto download = keyData ? client.partialDownload(*this, handle, *keyData, length, offset) :
                              client.partialDownload(*this, handle, length, offset);

    // Couldn't create the download.
    if (!download)
        return completed(download.error()), nullptr;

    // Grab download.
    mDownload = std::move(*download);

    // Return the download to our caller.
    return mDownload;
}

void FileRangeContext::queue(FileFetchCallback callback)
{
    // Queue the callback for later execution.
    mCallbacks.emplace_back(std::move(callback));
}

void FileRangeContext::queue(FileReadRequest request)
{
    // Request isn't dispatchable so queue it for later execution.
    if (!dispatch(request))
        mRequests.emplace(std::move(request));
}

bool retryable(const Error& result)
{
    // Client's being torn down or the download has been cancelled.
    if (result == API_EINCOMPLETE)
        return false;

    // File's been taken down because it breached our terms and conditions.
    if (result == API_ETOOMANY && result.hasExtraInfo())
        return false;

    // Retry all other failures.
    return true;
}

} // file_service
} // mega
