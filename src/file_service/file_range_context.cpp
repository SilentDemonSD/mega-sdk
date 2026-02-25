#include <mega/common/client.h>
#include <mega/common/database.h>
#include <mega/common/expected.h>
#include <mega/common/partial_download.h>
#include <mega/common/scoped_query.h>
#include <mega/common/task_executor.h>
#include <mega/common/transaction.h>
#include <mega/file_service/buffer.h>
#include <mega/file_service/displaced_buffer.h>
#include <mega/file_service/file_buffer.h>
#include <mega/file_service/file_context.h>
#include <mega/file_service/file_info_context.h>
#include <mega/file_service/file_range.h>
#include <mega/file_service/file_range_context.h>
#include <mega/file_service/file_range_tree_utilities.h>
#include <mega/file_service/file_read_request.h>
#include <mega/file_service/file_result.h>
#include <mega/file_service/file_service_context.h>
#include <mega/file_service/file_service_options.h>
#include <mega/file_service/logging.h>
#include <mega/file_service/memory_buffer.h>
#include <mega/types.h>

#include <cassert>

namespace mega
{
namespace file_service
{

using namespace common;

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
    [[maybe_unused]] auto self = std::move(mIterator->second);

    // Acquire lock.
    std::lock_guard guard(mContext.mLock);

    // What part of our original range do we still need to download?
    FileRange range(mEnd, mIterator->first.mEnd);

    // Remove ourselves from our file's map of downloading ranges.
    mContext.mDownloading.remove(mIterator);

    // Translate SDK error code to a file result.
    auto result = fileResultFromError(error);

    // Download didn't complete successfully.
    if (result != FILE_SUCCESS)
        mContext.failed(range, result);

    // Convenience.
    auto& executor = mContext.mService.executor();

    // Let any waiters know this range's download has completed.
    for (auto& callback: mCallbacks)
        executor.execute(std::bind(std::move(callback), result), true);
}

auto FileRangeContext::data(const void* buffer,
                            std::uint64_t offset,
                            std::uint64_t length,
                            const Speeds&) -> std::variant<Abort, Continue>
try
{
    // Convenience.
    auto* bytebuffer = static_cast<const std::uint8_t*>(buffer);
    auto& onDisk = mContext.mOnDisk;

    // Acquire lock.
    std::lock_guard guard(mContext.mLock);

    // Downloaded data is outside of this range.
    if (offset >= mIterator->first.mEnd)
        return Abort();

    // Original range of our write.
    FileRange range(offset, std::min(offset + length, mIterator->first.mEnd));

    // Effective range our write.
    FileRange written(offset, offset);

    // Convenience.
    auto current = gaps(onDisk, range);
    auto end = current.end();

    // Write data to each gap in range.
    for (; current != end; ++current)
    {
        offset = current->mBegin;
        length = current->mEnd - offset;

        // Try and write data to disk.
        auto [count, _] =
            mContext.mBuffer->write(bytebuffer + (offset - range.mBegin), offset, length);

        // Bump end of written range.
        written.mEnd = offset + count;

        // Couldn't write all of the data to disk.
        if (count < length)
            break;
    }

    // We didn't fill any gaps.
    if (written.mBegin == written.mEnd)
    {
        // Because no gaps needed to be filled.
        if (current == end)
            return Continue();

        // Because we couldn't write any data to the first gap.
        return Abort();
    }

    // Extend written range if all gaps were filled.
    written = current == end ? range : written;

    // Convenience.
    auto& database = mContext.mService.database();

    // Acquire database lock.
    std::lock_guard databaseLock(database);

    // Begin a transaction so we can safely modify the database.
    auto transaction = database.transaction();

    // Update ranges on disk and in memory.
    mContext.updateRanges(written, transaction);

    // Persist database changes.
    transaction.commit();

    // Bump end of written range.
    mEnd = written.mEnd;

    // Dispatch requests we can now satisfy.
    mContext.completed(written);

    // Couldn't write all of the data to disk.
    if (current != end)
        return Abort();

    // Continue the download.
    return Continue();
}

catch (std::runtime_error&)
{
    return Abort();
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
    return options.mRangeRetryBackoff * (1 << --retries);
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
    mIterator(iterator)
{}

FileRangeContext::~FileRangeContext() = default;

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

std::uint64_t FileRangeContext::end() const
{
    return mEnd;
}

void FileRangeContext::queue(FileFetchCallback callback)
{
    // Queue the callback for later execution.
    mCallbacks.emplace_back(std::move(callback));
}

void FileRangeContext::range(std::uint64_t begin, std::uint64_t end)
{
    range(FileRange(begin, end));
}

void FileRangeContext::range(const FileRange& range)
{
    // Range hasn't actually changed.
    if (mIterator->first == range)
        return;

    // Keep ourselves alive.
    auto context = std::move(mIterator->second);

    // Convenience.
    auto& downloading = mContext.mDownloading;

    // Remove ourselves from our file's map of downloading ranges.
    downloading.remove(mIterator);

    [[maybe_unused]] auto added = false;

    // Add ourselves back into our file's map of downloading ranges.
    std::tie(mIterator, added) = downloading.tryAdd(range, std::move(context));

    // Sanity: The context should always be placed back into the map.
    assert(added);

    // Ensure end of downloaded data is within our new range.
    mEnd = std::min(range.mEnd, std::max(range.mBegin, mEnd));
}

} // file_service
} // mega
