#include "mega/auto_file_handle.h"

#ifdef WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace mega
{

void OsFileDescriptorTraits::close(OsFileDescriptorTraits::Type h)
{
#ifdef WIN32
    ::CloseHandle(h);
#else
    ::close(h);
#endif
}

} // namespace mega
