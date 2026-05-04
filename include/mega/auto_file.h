#pragma once
#include <utility>

namespace mega
{

template<typename FileTraits>
class AutoFile
{
    using HandleType = typename FileTraits::Type;
    const HandleType UNSET = FileTraits::InvalidValue;
    HandleType h = UNSET;

public:
    AutoFile() {}

    AutoFile(HandleType ih):
        h(ih)
    {}

    ~AutoFile()
    {
        close();
    }

    void close()
    {
        if (h != UNSET)
        {
            FileTraits::close(h);
        }

        h = UNSET;
    }

    AutoFile& operator=(HandleType ih)
    {
        // avoid to leak a handle if changed
        if (ih != h)
            close();

        h = ih;
        return *this;
    }

    bool isSet() const
    {
        return h != UNSET;
    }

    operator bool() const
    {
        return isSet();
    }

    // implicit conversion, so can pass into OS API
    operator HandleType() const
    {
        return h;
    }

    HandleType* ptr()
    {
        return &h;
    }

    HandleType get() const
    {
        return h;
    }

    HandleType release()
    {
        return std::exchange(h, UNSET);
    }

    // Prevent copying to avoid double-close issues
    AutoFile(const AutoFile&) = delete;
    AutoFile& operator=(const AutoFile&) = delete;

    // Allow moving
    AutoFile(AutoFile&& other) noexcept:
        h(other.h)
    {
        other.h = UNSET;
    }

    AutoFile& operator=(AutoFile&& other) noexcept
    {
        if (this != &other)
        {
            close();
            h = std::exchange(other.h, UNSET);
        }
        return *this;
    }
};

} // namespace mega
