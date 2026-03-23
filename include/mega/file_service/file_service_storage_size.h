#pragma once
#include <cstdint>

namespace mega
{

namespace file_service
{

struct StorageSize
{
    // How many bytes reclaimable
    std::uint64_t mSizeToReclaim = 0;

    // How many bytes the storage allocates physically
    std::uint64_t mTotalAllocatedSize = 0;

    // How many bytes the storage reports, usually not less than mTotalAllocatedSize
    std::uint64_t mTotalReportedSize = 0;

    // How many bytes the cloud reports
    std::uint64_t mTotalSize = 0;

    bool operator==(const StorageSize& other) const
    {
        return mSizeToReclaim == other.mSizeToReclaim &&
               mTotalAllocatedSize == other.mTotalAllocatedSize &&
               mTotalReportedSize == other.mTotalReportedSize && mTotalSize == other.mTotalSize;
    }
};

} // file_service
} // mega
