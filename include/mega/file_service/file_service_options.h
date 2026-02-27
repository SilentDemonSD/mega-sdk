#pragma once

#include <mega/common/deciseconds.h>
#include <mega/file_service/file_service_options_forward.h>

#include <chrono>
#include <cstdint>
#include <optional>

namespace mega
{
namespace file_service
{

struct ReclaimOptions
{
    // How long shouldn't we access a file before we can reclaim it?
    std::chrono::hours mAgeThreshold{3 * 24};

    // How many files should we reclaim at a time?
    std::size_t mBatchSize = 4u;

    // How long after startup should we wait until we reclaim space?
    std::chrono::seconds mDelay{30 * 60};

    // How often should we try to reclaim space?
    std::chrono::seconds mPeriod{2 * 60 * 60};

    // How many bytes can the service store before it needs to reclaim space?
    std::uint64_t mSizeThreshold{0};
};

struct ServiceOptions
{
    // How long should we wait before we remove a file context from memory?
    std::chrono::seconds mFileContextReleaseDelay{2 * 60};

    // How many times will we try to download a range before we give up.
    std::uint64_t mMaximumRangeRetries = 5u;

    // How long should we wait between retries?
    common::deciseconds mRangeRetryBackoff{20};
}; // ServiceOptions

} // file_service
} // mega
