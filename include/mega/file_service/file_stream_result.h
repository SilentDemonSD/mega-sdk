#pragma once

#include <mega/file_service/file_callbacks.h>
#include <mega/file_service/file_stream_result_forward.h>

namespace mega
{
namespace file_service
{

struct FileStreamResult
{
    // Call to continue streaming data from the file.
    FileStreamContinueCallback mContinue;

    // The data most recently streamed from the file.
    const void* mBuffer = nullptr;

    // How many bytes of data mBuffer contains.
    std::uint64_t mLength = 0ul;
}; // FileStreamResult

} // file_service
} // mega
