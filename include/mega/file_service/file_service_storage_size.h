#pragma once
#include <cstdint>

namespace mega
{

namespace file_service
{

struct StorageInfo
{
    // How many bytes reclaimable
    std::uint64_t mReclaimableSize = 0;

    // How many bytes the storage allocates physically
    std::uint64_t mAllocatedSize = 0;

    // How many bytes the storage reports, usually not less than mAllocatedSize
    std::uint64_t mReportedSize = 0;

    // How many bytes the cloud reports
    std::uint64_t mTotalSize = 0;

    bool operator==(const StorageInfo& other) const
    {
        return mReclaimableSize == other.mReclaimableSize &&
               mAllocatedSize == other.mAllocatedSize && mReportedSize == other.mReportedSize &&
               mTotalSize == other.mTotalSize;
    }
};

} // file_service
} // mega
