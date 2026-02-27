#include <mega/common/lock.h>
#include <mega/file_service/file.h>
#include <mega/file_service/file_id.h>
#include <mega/file_service/file_info.h>
#include <mega/file_service/file_service.h>
#include <mega/file_service/file_service_context.h>
#include <mega/file_service/file_service_options.h>
#include <mega/file_service/file_service_result.h>
#include <mega/file_service/file_service_result_or.h>
#include <mega/file_service/logging.h>

#include <stdexcept>

namespace mega
{
namespace file_service
{

using namespace common;

FileService::FileService():
    mInstanceLogger("FileService", *this, logger()),
    mContext(),
    mContextLock()
{}

FileService::~FileService() = default;

auto FileService::add(NodeHandle handle, const NodeKeyData& keyData, std::size_t size)
    -> FileServiceResultOr<FileID>
{
    SharedLock guard(mContextLock);

    if (mContext)
        return mContext->add(handle, keyData, size);

    return unexpected(FILE_SERVICE_UNINITIALIZED);
}

auto FileService::addObserver(FileEventObserver observer)
    -> FileServiceResultOr<FileEventObserverID>
{
    SharedLock guard(mContextLock);

    if (mContext)
        return mContext->addObserver(std::move(observer));

    return unexpected(FILE_SERVICE_UNINITIALIZED);
}

auto FileService::create(NodeHandle parent, const std::string& name) -> FileServiceResultOr<File>
{
    SharedLock guard(mContextLock);

    if (!mContext)
        return unexpected(FILE_SERVICE_UNINITIALIZED);

    return mContext->create(parent, name);
}

void FileService::deinitialize()
{
    UniqueLock guard(mContextLock);

    mContext.reset();
}

auto FileService::info(FileID id) -> FileServiceResultOr<FileInfo>
{
    SharedLock guard(mContextLock);

    if (!mContext)
        return unexpected(FILE_SERVICE_UNINITIALIZED);

    if (!id)
        return unexpected(FILE_SERVICE_FILE_DOESNT_EXIST);

    return mContext->info(id);
}

auto FileService::open(NodeHandle parent, const std::string& name) -> FileServiceResultOr<File>
{
    SharedLock guard(mContextLock);

    if (!mContext)
        return unexpected(FILE_SERVICE_UNINITIALIZED);

    return mContext->open(parent, name);
}

auto FileService::open(FileID id) -> FileServiceResultOr<File>
{
    SharedLock guard(mContextLock);

    if (!mContext)
        return unexpected(FILE_SERVICE_UNINITIALIZED);

    if (!id)
        return unexpected(FILE_SERVICE_FILE_DOESNT_EXIST);

    return mContext->open(id);
}

auto FileService::serviceOptions(const ServiceOptions& serviceOptions) -> FileServiceResult
{
    SharedLock guard(mContextLock);

    if (!mContext)
        return FILE_SERVICE_UNINITIALIZED;

    mContext->serviceOptions(serviceOptions);

    return FILE_SERVICE_SUCCESS;
}

auto FileService::serviceOptions() -> FileServiceResultOr<ServiceOptions>
{
    SharedLock guard(mContextLock);

    if (!mContext)
        return unexpected(FILE_SERVICE_UNINITIALIZED);

    return mContext->serviceOptions();
}

auto FileService::initialize(Client& client,
                             const ServiceOptions& serviceOptions,
                             const ReclaimOptions& reclaimOptions) -> FileServiceResult
try
{
    UniqueLock guard(mContextLock);

    if (mContext)
    {
        FSError1("File Service has already been initialized");

        return FILE_SERVICE_ALREADY_INITIALIZED;
    }

    mContext = std::make_unique<FileServiceContext>(client, serviceOptions, reclaimOptions);

    FSInfo1("File Service initialized");

    return FILE_SERVICE_SUCCESS;
}
catch (std::runtime_error& exception)
{
    FSErrorF("Unable to initialize File Service: %s", exception.what());

    return FILE_SERVICE_UNEXPECTED;
}

auto FileService::initialize(Client& client) -> FileServiceResult
{
    return initialize(client, ServiceOptions(), ReclaimOptions());
}

auto FileService::purge() -> FileServiceResult
{
    SharedLock guard(mContextLock);

    if (!mContext)
        return FILE_SERVICE_UNINITIALIZED;

    return mContext->purge();
}

void FileService::reclaim(ReclaimCallback callback)
{
    SharedLock guard(mContextLock);

    if (!mContext)
        return callback(FILE_SERVICE_UNINITIALIZED);

    return mContext->reclaim(std::move(callback));
}

auto FileService::reclaimOptions(const ReclaimOptions& options) -> FileServiceResult
{
    SharedLock guard(mContextLock);

    if (!mContext)
        return FILE_SERVICE_UNINITIALIZED;

    mContext->reclaimOptions(options);

    return FILE_SERVICE_SUCCESS;
}

auto FileService::reclaimOptions() -> FileServiceResultOr<ReclaimOptions>
{
    SharedLock guard(mContextLock);

    if (!mContext)
        return unexpected(FILE_SERVICE_UNINITIALIZED);

    return mContext->reclaimOptions();
}

auto FileService::removeObserver(FileEventObserverID id) -> FileServiceResult
{
    SharedLock guard(mContextLock);

    if (!mContext)
        return FILE_SERVICE_UNINITIALIZED;

    mContext->removeObserver(id);

    return FILE_SERVICE_SUCCESS;
}

auto FileService::storageUsed() -> FileServiceResultOr<std::uint64_t>
{
    SharedLock guard(mContextLock);

    if (!mContext)
        return unexpected(FILE_SERVICE_UNINITIALIZED);

    return mContext->storageUsed();
}

} // file_service
} // mega
