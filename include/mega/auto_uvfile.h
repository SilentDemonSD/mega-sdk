#pragma once

#include "mega/auto_file.h"

#include <uv.h>

namespace mega
{
struct UVFileTraits
{
    using Type = uv_file;
    static inline const Type InvalidValue = -1;
    static void close(Type h);
};

using AutoUVFile = AutoFile<UVFileTraits>;
} // namespace mega
