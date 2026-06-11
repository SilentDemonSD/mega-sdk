#pragma once

#include <mega/fuse/common/mount_result.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace mega
{
namespace fuse
{

/**
 * @brief
 * Exception carrying a FUSE mount result code.
 */
class MountException final: public std::runtime_error
{
    MountResult mResult;
    int mErrorCode;

public:
    /**
     * @brief
     * Constructs an exception with a mount result and text.
     *
     * @param result
     * FUSE mount result code associated with the failure.
     *
     * @param message
     * Human readable description of the failure.
     */
    MountException(MountResult result, int errorCode, std::string message):
        std::runtime_error(std::move(message)),
        mResult(result),
        mErrorCode(errorCode)
    {}

    /**
     * @brief
     * Retrieves the FUSE mount result carried by this exception.
     *
     * @return
     * Stored mount result code.
     */
    MountResult result() const noexcept
    {
        return mResult;
    }

    /**
     * @brief
     * Retrieves the OS error code associated with the failure.
     *
     * @return
     * Stored OS error code.
     */
    int errorCode() const noexcept
    {
        return mErrorCode;
    }
};

/**
 * @brief
 * Creates and logs a mount exception using a printf-style format.
 *
 * @param errorCode
 * Platform-specific error code associated with the failure.
 *
 * @param file
 * Source filename where the exception is being created.
 *
 * @param line
 * Source line where the exception is being created.
 *
 * @param format
 * printf-style message format.
 *
 * @return
 * Fully initialized mount exception.
 */
MountException
    makeMountException(int errorCode, const char* file, unsigned int line, const char* format, ...);

#define FUSEMountError1(errorCode, format) \
    ::mega::fuse::makeMountException((errorCode), __FILE__, __LINE__, (format))

#define FUSEMountErrorF(errorCode, format, ...) \
    ::mega::fuse::makeMountException((errorCode), __FILE__, __LINE__, (format), __VA_ARGS__)

} // fuse
} // mega
