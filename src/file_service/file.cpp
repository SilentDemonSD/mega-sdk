#include <mega/file_service/file.h>
#include <mega/file_service/file_append_request.h>
#include <mega/file_service/file_context.h>
#include <mega/file_service/file_fetch_request.h>
#include <mega/file_service/file_flush_request.h>
#include <mega/file_service/file_info.h>
#include <mega/file_service/file_range.h>
#include <mega/file_service/file_read_request.h>
#include <mega/file_service/file_read_result.h>
#include <mega/file_service/file_remove_request.h>
#include <mega/file_service/file_result.h>
#include <mega/file_service/file_result_or.h>
#include <mega/file_service/file_service_context_badge.h>
#include <mega/file_service/file_stream_result.h>
#include <mega/file_service/file_touch_request.h>
#include <mega/file_service/file_truncate_request.h>
#include <mega/file_service/file_write_request.h>
#include <mega/file_service/logger.h>
#include <mega/file_service/source.h>

#include <utility>
#include <vector>

namespace mega
{
namespace file_service
{

class StreamContext;

using StreamContextPtr = std::shared_ptr<StreamContext>;

class StreamContext
{
    // Called when we've received file data.
    void onData(StreamContextPtr& context,
                std::uint64_t length,
                std::uint64_t offset,
                FileResultOr<FileReadResult> result);

    // Called when the user wants to stream more file data.
    void onContinue(StreamContextPtr& context,
                    std::uint64_t consumed,
                    std::uint64_t length,
                    std::uint64_t offset);

    // Where we will temporarily store file data.
    std::vector<char> mBuffer;

    // Who should we call with the file's data?
    FileStreamDataCallback mCallback;

    // What file are we streaming?
    File mFile;

public:
    StreamContext(FileStreamDataCallback callback, File file);

    // Start streaming file data.
    void stream(StreamContextPtr context, std::uint64_t offset, std::uint64_t length);
}; // StreamContext

File::File(FileServiceContextBadge, FileContextPtr context):
    mInstanceLogger("File", *this, logger()),
    mContext(std::move(context))
{}

File::~File() = default;

File::File(const File& other):
    mInstanceLogger("File", *this, logger()),
    mContext(other.mContext)
{}

File::File(File&& other):
    mInstanceLogger("File", *this, logger()),
    mContext(std::exchange(other.mContext, nullptr))
{}

File& File::operator=(const File& rhs)
{
    if (this != &rhs)
        mContext = rhs.mContext;

    return *this;
}

File& File::operator=(File&& rhs)
{
    using std::swap;

    if (this != &rhs)
        swap(mContext, rhs.mContext);

    return *this;
}

FileEventObserverID File::addObserver(FileEventObserver observer)
{
    assert(mContext);

    return mContext->addObserver(std::move(observer));
}

void File::append(const void* buffer, FileAppendCallback callback, std::uint64_t length)
{
    assert(buffer || !length);
    assert(callback);
    assert(mContext);

    return mContext->append(FileAppendRequest{buffer, std::move(callback), length});
}

void File::fetch(FileFetchCallback callback)
{
    assert(callback);
    assert(mContext);

    mContext->fetch(FileFetchRequest{std::move(callback)});
}

void File::fetchBarrier(FileFetchBarrierCallback callback)
{
    assert(callback);
    assert(mContext);

    mContext->fetchBarrier(std::move(callback));
}

void File::flush(FileFlushCallback callback)
{
    assert(callback);
    assert(mContext);

    return mContext->flush(FileFlushRequest{std::move(callback)});
}

FileInfo File::info() const
{
    assert(mContext);

    return mContext->info();
}

void File::purge(FilePurgeCallback callback)
{
    assert(callback);
    assert(mContext);

    return mContext->remove(FileRemoveRequest{std::move(callback), false, true});
}

FileRangeVector File::ranges() const
{
    assert(mContext);

    return mContext->ranges();
}

void File::read(FileReadCallback callback, std::uint64_t offset, std::uint64_t length)
{
    assert(callback);
    assert(mContext);

    read(std::move(callback), FileRange(offset, offset + length));
}

void File::read(FileReadCallback callback, const FileRange& range)
{
    assert(callback);
    assert(mContext);

    mContext->read(FileReadRequest{std::move(callback), range});
}

void File::reclaim(FileReclaimCallback callback)
{
    assert(callback);
    assert(mContext);

    mContext->reclaim(std::move(callback));
}

void File::remove(FileRemoveCallback callback, bool replaced)
{
    assert(callback);
    assert(mContext);

    mContext->remove(FileRemoveRequest{std::move(callback), replaced, false});
}

void File::removeObserver(FileEventObserverID id)
{
    assert(mContext);

    mContext->removeObserver(id);
}

void File::stream(FileStreamDataCallback callback, std::uint64_t offset, std::uint64_t length)
{
    // Sanity.
    assert(callback);

    // Instantiate streaming context.
    auto context = std::make_shared<StreamContext>(std::move(callback), *this);

    // Start streaming data.
    context->stream(context, offset, length);
}

void File::touch(FileTouchCallback callback, std::int64_t modified)
{
    assert(callback);
    assert(mContext);

    mContext->touch(FileTouchRequest{std::move(callback), modified});
}

void File::truncate(FileTruncateCallback callback, std::uint64_t newSize)
{
    assert(callback);
    assert(mContext);

    mContext->truncate(FileTruncateRequest{std::move(callback), newSize});
}

void File::write(const void* buffer,
                 FileWriteCallback callback,
                 std::uint64_t offset,
                 std::uint64_t length)
{
    write(buffer, std::move(callback), FileRange(offset, offset + length));
}

void File::write(const void* buffer, FileWriteCallback callback, const FileRange& range)
{
    assert(buffer || range.mEnd - range.mBegin == 0);
    assert(callback);
    assert(mContext);

    mContext->write(FileWriteRequest{buffer, std::move(callback), range});
}

void StreamContext::onData(StreamContextPtr& context,
                           std::uint64_t length,
                           std::uint64_t offset,
                           FileResultOr<FileReadResult> result)
{
    // Sanity.
    assert(context.get() == this);

    // Couldn't stream file data.
    if (!result)
        return mCallback(unexpected(result.error()));

    // No further data needs to be streamed.
    if (!result->mLength)
        return mCallback(FileStreamResult{});

    // How much data do we want to store in our buffer?
    auto count = std::min<std::uint64_t>(mBuffer.size(), result->mLength);

    // Try and transfer the streamed data into our buffer.
    std::tie(count, std::ignore) = result->mSource.read(mBuffer.data(), 0, count);

    // Couldn't read any data from the file.
    if (!count)
        return mCallback(unexpected(FILE_FAILED));

    // Pass streamed data to the user.
    mCallback(FileStreamResult{std::bind(&StreamContext::onContinue,
                                         this,
                                         std::move(context),
                                         std::placeholders::_1,
                                         length,
                                         offset),
                               mBuffer.data(),
                               count});
}

void StreamContext::onContinue(StreamContextPtr& context,
                               std::uint64_t consumed,
                               std::uint64_t length,
                               std::uint64_t offset)
{
    // Sanity.
    assert(context.get() == this);

    // Make sure consumed is sane.
    consumed = std::min(consumed, length);

    // Try and stream some more file data.
    stream(std::move(context), offset + consumed, length - consumed);
}

StreamContext::StreamContext(FileStreamDataCallback callback, File file):
    mBuffer(128 * 1024),
    mCallback(std::move(callback)),
    mFile(std::move(file))
{}

void StreamContext::stream(StreamContextPtr context, std::uint64_t offset, std::uint64_t length)
{
    // Sanity.
    assert(context.get() == this);

    // Try and stream some data from the file.
    mFile.read(std::bind(&StreamContext::onData,
                         this,
                         std::move(context),
                         length,
                         offset,
                         std::placeholders::_1),
               offset,
               length);
}

} // file_service
} // mega
