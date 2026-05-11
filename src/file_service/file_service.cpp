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

auto FileService::add(NodeHandle handle,
                      const NodeKeyData& keyData,
                      std::uint64_t size) -> FileServiceResultOr<FileID>
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

auto FileService::databasePath() const -> FileServiceResultOr<LocalPath>
{
    SharedLock guard(mContextLock);

    if (mContext)
        return mContext->databasePath();

    return unexpected(FILE_SERVICE_UNINITIALIZED);
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
                             const ReclaimOptions& reclaimOptions,
                             const ServiceOptions& serviceOptions) -> FileServiceResult
try
{
    UniqueLock guard(mContextLock);

    if (mContext)
    {
        FSError1("File Service has already been initialized");

        return FILE_SERVICE_ALREADY_INITIALIZED;
    }

    mContext = std::make_unique<FileServiceContext>(client, reclaimOptions, serviceOptions);

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
    return initialize(client, ReclaimOptions(), ServiceOptions());
}

auto FileService::purge() -> FileServiceResult
{
    SharedLock guard(mContextLock);

    if (!mContext)
        return FILE_SERVICE_UNINITIALIZED;

    return mContext->purge();
}

auto FileService::reclaim(ReclaimCallback callback,
                          std::optional<ReclaimOptions> reclaimOptions) -> FileServiceResult
{
    SharedLock guard(mContextLock);

    if (!mContext)
        return FILE_SERVICE_UNINITIALIZED;

    // Convenience.
    auto& executor = mContext->executor();

    executor.execute(
        [this, callback = std::move(callback), reclaimOptions = std::move(reclaimOptions)](
            const common::Task&)
        {
            mContext->reclaim(std::move(callback),
                              reclaimOptions.value_or(mContext->reclaimOptions()));
        },
        true);

    return FILE_SERVICE_SUCCESS;
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

auto FileService::storageInfo(const ReclaimOptions* options) -> FileServiceResultOr<StorageInfo>
{
    SharedLock guard(mContextLock);

    if (!mContext)
        return unexpected(FILE_SERVICE_UNINITIALIZED);

    return mContext->storageInfo(options);
}

auto FileService::storageUsed() -> FileServiceResultOr<std::uint64_t>
{
    SharedLock guard(mContextLock);

    if (!mContext)
        return unexpected(FILE_SERVICE_UNINITIALIZED);

    return mContext->storageUsed();
}

auto FileService::userFilePath(FileID id) const -> FileServiceResultOr<LocalPath>
{
    SharedLock guard(mContextLock);

    if (mContext)
        return mContext->userFilePath(id);

    return unexpected(FILE_SERVICE_UNINITIALIZED);
}

} // file_service
} // mega
