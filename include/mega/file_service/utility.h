#pragma once

#include <mega/file_service/file_callbacks.h>
#include <mega/file_service/file_forward.h>
#include <mega/file_service/file_result_or_forward.h>
#include <mega/file_service/file_stream_result_forward.h>

#include <cstdint>

namespace mega
{
namespace file_service
{

// Stream data from the specified file.
void stream(FileStreamDataCallback callback, File file, std::uint64_t offset, std::uint64_t length);

// Stream data from the specified file.
void stream(FileStreamFDCallback callback, File file, std::uint64_t offset, std::uint64_t length);

} // file_service
} // mega
