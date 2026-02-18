#pragma once

#include <mega/common/activity_monitor.h>
#include <mega/common/client_forward.h>
#include <mega/common/instance_logger.h>
#include <mega/common/node_key_data.h>
#include <mega/common/partial_download_callback.h>
#include <mega/common/partial_download_forward.h>
#include <mega/file_service/buffer_pointer.h>
#include <mega/file_service/file_buffer_pointer.h>
#include <mega/file_service/file_callbacks.h>
#include <mega/file_service/file_context_forward.h>
#include <mega/file_service/file_range_context_forward.h>
#include <mega/file_service/file_range_map.h>
#include <mega/file_service/file_read_request_forward.h>
#include <mega/file_service/file_read_request_set.h>

#include <cstdint>
#include <mutex>

namespace mega
{

class Error;
class NodeHandle;
struct FileAccess;

namespace file_service
{

class FileRangeContext: private common::PartialDownloadCallback
{
    // Called when the file range has been downloaded.
    void completed(Error result) override;

    // Called repeatedly as data is donwloaded from the cloud.
    auto data(const void* buffer,
              std::uint64_t offset,
              std::uint64_t length,
              const Speeds&) -> std::variant<Abort, Continue> override;

    // Dispatch zero or more read requests.
    void dispatch();

    // Try and dispatch the specified request.
    //
    // Returns true if the request was dispatched.
    bool dispatch(FileReadRequest& request);

    // Called when our download's encountered a failure.
    virtual auto failed(Error result, int retries) -> std::variant<Abort, Retry> override;

    // Logs instance lifetime.
    common::InstanceLogger<FileRangeContext> mInstanceLogger;

    // Keeps our manager alive until we're dead.
    common::Activity mActivity;

    // Callbacks to execute when this range's fetch completes.
    std::vector<FileFetchCallback> mCallbacks;

    // Which file is responsible for this context?
    FileContext& mContext;

    // The download that's retrieving this file range's data.
    common::PartialDownloadPtr mDownload;

    // Where does our downloaded data currently end?
    std::uint64_t mEnd;

    // Where are we in our manager's map of contexts?
    FileRangeMap<FileRangeContextPtr>::Iterator mIterator;

    // Requests pending completion.
    FileReadRequestSet mRequests;

public:
    FileRangeContext(common::Activity activity,
                     FileContext& context,
                     FileRangeMap<FileRangeContextPtr>::Iterator iterator);

    ~FileRangeContext();

    // Cancel this range's download.
    void cancel();

    // Create a download this range.
    auto download() -> common::PartialDownloadPtr;

    // Queue a callback for execution when this range has downloaded.
    void queue(FileFetchCallback callback);

    // Queue a file read request for later completion.
    void queue(FileReadRequest request);
}; // FileRangeContext

} // file_service
} // mega
