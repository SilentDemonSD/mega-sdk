#pragma once

#include <cstdint>
#include <utility>

namespace mega
{
namespace file_service
{

using FileEventObserverID = std::pair<FileEventEmitter*, std::uint64_t>;

} // file_service
} // mega
