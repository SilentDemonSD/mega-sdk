#include <mega/common/utility.h>
#include <mega/fuse/common/logger.h>
#include <mega/fuse/common/mount_exception.h>

#include <cerrno>
#include <cstdarg>

namespace mega
{
namespace fuse
{

MountException
    makeMountException(int errorCode, const char* file, unsigned int line, const char* format, ...)
{
    std::va_list arguments;
    va_start(arguments, format);
    auto message = common::formatv(arguments, format);
    va_end(arguments);

    logger().error(file, "%s", line, message.c_str());

    auto toMountResult = [](int errorCode) -> MountResult
    {
#ifdef _WIN32
        (void)errorCode;
        return MOUNT_UNEXPECTED;
#else
        switch (errorCode)
        {
            case ENOTCONN:
            case ESTALE:
                return MOUNT_LOCAL_STALE;
            default:
                return MOUNT_UNEXPECTED;
        }
#endif
    };

    auto result = toMountResult(errorCode);
    return MountException(result, errorCode, std::move(message));
}

} // fuse
} // mega
