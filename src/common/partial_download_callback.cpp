#include <mega/common/partial_download_callback.h>
#include <mega/types.h>

namespace mega
{
namespace common
{

bool PartialDownloadCallback::retryable(const Error& result)
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

} // common
} // mega
