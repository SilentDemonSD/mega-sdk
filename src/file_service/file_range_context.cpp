#include <mega/common/client.h>
#include <mega/common/expected.h>
#include <mega/common/partial_download.h>
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

void FileRangeContext::completed(Error result)
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

    // Convenience.
    FileRange range(mIterator->first.mBegin, mEnd);

    // Let the manager know this download has completed.
    mContext.completed(mIterator, range);

    // Complete as many requests as we can.
    dispatch();

    // Translate SDK result.
    auto result_ = fileResultFromError(result);

    // Download didn't complete successfully.
    if (result_ != FILE_SUCCESS)
    {
        // Fail any remaining requests.
        for (auto i = mRequests.begin(); i != mRequests.end();)
        {
            // Convenience.
            auto& request = const_cast<FileReadRequest&>(*i);

            // Fail the request.
            mContext.completed(std::move(request), result_);

            // Remove the request from our set.
            i = mRequests.erase(i);
        }
    }

    // Let any waiters know this range's download has completed.
    for (auto& callback: mCallbacks)
        mContext.mService.execute(std::bind(std::move(callback), result_));
}

auto FileRangeContext::data(const void* buffer,
                            std::uint64_t offset,
                            std::uint64_t length,
                            const Speeds&) -> std::variant<Abort, Continue>
{
    // Try and write data to our buffer.
    auto [count, success] = mContext.mBuffer->write(buffer, offset, length);

    // Lock our manager.
    std::unique_lock lock(mContext.mDownloadingLock);

    // Bump our buffer iterator.
    mEnd += count;

    // Couldn't write all of the data to our buffer.
    if (!success)
        return Abort();

    // Don't dispatch any requests here if this is the last piece of the
    // file. Instead, dispatch them when the download is completed.
    //
    // This is necessary to stabilize the integration tests as they expect
    // all necessary processing to have completed by the time any final read
    // callbacks have been executed.
    if (mEnd == mIterator->first.mEnd)
        return Continue();

    // Dispatch what requests we can.
    dispatch();

    // Let the caller know the download should continue.
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
