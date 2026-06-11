#include <mega/common/utility.h>
#include <mega/file_service/file_range.h>

#include <cassert>
#include <cinttypes>
#include <utility>

namespace mega
{
namespace file_service
{

using namespace common;

FileRange::FileRange(std::uint64_t begin, std::uint64_t end):
    mBegin(std::min(begin, end)),
    mEnd(std::max(begin, end))
{}

FileRange clamp(const FileRange& range, std::uint64_t begin, std::uint64_t end)
{
    // Sanity: begin must be less than or equal to end.
    assert(begin <= end);

    auto begin_ = std::max(std::min(range.mBegin, end), begin);
    auto end_ = std::max(std::min(range.mEnd, end), begin);

    return FileRange(begin_, end_);
}

FileRange combine(const FileRange& lhs, const FileRange& rhs)
{
    auto begin = std::min(lhs.mBegin, rhs.mBegin);
    auto end = std::max(lhs.mEnd, rhs.mEnd);

    return FileRange(begin, end);
}

FileRange extend(const FileRange& range, std::uint64_t adjustment)
{
    auto begin = range.mBegin - std::min(adjustment, range.mBegin);
    auto end = range.mEnd + adjustment;

    return FileRange(begin, end);
}

std::string toString(const FileRange& range)
{
    return format("[%" PRIu64 "-%" PRIu64 "]", range.mBegin, range.mEnd);
}

std::ostream& operator<<(std::ostream& ostream, const FileRange& range)
{
    return ostream << toString(range);
}

} // file_service
} // mega
