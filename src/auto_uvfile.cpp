#include "mega/auto_uvfile.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace mega
{

void UVFileTraits::close(UVFileTraits::Type h)
{
#ifdef _WIN32
    _close(h);
#else
    ::close(h);
#endif
}

} // namespace mega
