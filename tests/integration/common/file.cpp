#include <mega/common/testing/file.h>
#include <mega/common/testing/utility.h>
#include <mega/logging.h>

#include <fstream>
#include <utility>

namespace mega
{
namespace common
{
namespace testing
{

File::File(const std::string& content, const std::string& name, const Path& parentPath):
    mPath(parentPath.path() / u8path_compat(name))
{
    std::ofstream ostream;

    // Throw on failure.
    ostream.exceptions(std::ios::badbit | std::ios::failbit);

    // Open file for writing.
    ostream.open(mPath.string(), std::ios::binary | std::ios::trunc);

    // Write data to the file.
    ostream.write(content.data(), static_cast<std::streamsize>(content.size()));

    // Flush content to disk.
    ostream.flush();
}

File::File(const std::string& content, const std::string& name):
    File(content, name, fs::current_path())
{}

File::File(std::uint64_t length, const std::string& name, const Path& parentPath):
    mPath(parentPath.path() / fs::u8path(name))
{
    // So we can create a new file on disk.
    std::ofstream ostream;

    // So IO failures result in an exception.
    ostream.exceptions(std::ios::badbit | std::ios::failbit);

    // Open our file for writing.
    ostream.open(mPath.string(), std::ios::binary | std::ios::trunc);

    // We'll write data to the file in chunks up to 1MiB.
    auto chunkSize = std::min<std::uint64_t>(1ul << 20, length);

    // Generate a chunk of random bytes.
    auto chunk = randomBytes(chunkSize);

    // Generate the file by repeatedly writing chunks to disk.
    for (; length > chunkSize; length -= chunkSize)
        ostream.write(chunk.data(), static_cast<std::streamsize>(chunkSize));

    // Write any remaining bytes to disk.
    //
    // Necessary when length isn't a multiple of chunkSize.
    if (length)
        ostream.write(chunk.data(), static_cast<std::streamsize>(length));

    // Flush content to disk.
    ostream.flush();
}

File::File(std::uint64_t length, const std::string& name):
    File(length, name, fs::current_path())
{}

File::~File()
{
    std::error_code error;

    // Try and remove the file.
    fs::remove(mPath, error);

    if (!error)
        return;

    LOG_warn << "Unable to remove file at: " << mPath.localPath();
}

const Path& File::path() const
{
    return mPath;
}

} // testing
} // fuse
} // mega
