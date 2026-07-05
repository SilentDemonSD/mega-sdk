#pragma once

#include "mega/auto_file.h"
#include "mega/types.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace mega
{

struct OsFileDescriptorTraits
{
    using Type = OsFileDescriptor;
    static inline const Type InvalidValue = INVALID_OS_FD;
    static void close(Type h);
};

using AutoFileHandle = AutoFile<OsFileDescriptorTraits>;

} // namespace mega
