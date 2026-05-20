#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

namespace mega
{
namespace common
{
namespace detail
{

// Latches an input value if it satisfies a given predicate.
template<template<typename T> typename Predicate, typename T>
class Latcher
{
    // Current latch value.
    std::optional<T> mValue{};

public:
    // Feed a new value into our latch.
    T operator()(T value)
    {
        // Latch value as needed.
        if (!mValue || Predicate<T>()(value, *mValue))
            mValue = value;

        // Return latched value.
        return *mValue;
    }

    // Retrieve the latch's current value.
    std::optional<T> get() const
    {
        return mValue;
    }
}; // Latcher<Predicate<?>, T>

} // detail

// Computes a running average over a window of at most N values.
template<typename T, std::size_t N>
class Averager
{
    // The current running average.
    std::optional<double> mAverage{};

    // At what index does our window of values begin?
    std::size_t mBegin{};

    // How many values does our window contain?
    std::size_t mCount{};

    // The current running total of the last N input values.
    T mTotal{};

    // Keeps track of the last N input values.
    T mValues[N]{};

public:
    // Add a new value into our running average.
    double operator()(T value)
    {
        // Compute where we'll store the new value.
        auto index = (mBegin + mCount) % N;

        // Window already contains N values so rotate it.
        if (mCount == N)
        {
            // Remove oldest value from running total.
            mTotal -= mValues[mBegin];

            // Rotate window forward by one value.
            mBegin = (mBegin + 1) % N;
        }

        // Add value to the end of our window.
        mValues[index] = value;

        // Add value into our running total.
        mTotal += value;

        // Bump counter as necessary.
        mCount = std::min(mCount + 1, N);

        // Recompute running average.
        mAverage = static_cast<double>(mTotal) / static_cast<double>(mCount);

        // Return updated running average.
        return *mAverage;
    }

    // Retrieve the current running average.
    std::optional<double> get() const
    {
        return mAverage;
    }
}; // Averager<T, N>

// Exponential Moving Average
class EmaInteger
{
public:
    // alpha: A value between 0 and 256 (where 256 = alpha of 1.0)
    // 26: Roughly alpha = 0.1 (Very smooth, slow to react).
    // 64: Exactly alpha = 0.25 (Balanced).
    // 128: Exactly alpha = 0.5 (Responsive, but shows more jitter).
    EmaInteger(uint32_t alpha):
        mAlpha(alpha)
    {}

    void update(uint64_t newSample)
    {
        // Shift new sample to match our internal fixed-point scale (2^8)
        uint64_t scaledSample = newSample << 8;

        if (!mCurrentEma)
        {
            mCurrentEma = scaledSample;
        }
        else
        {
            // EMA Formula: Current = (Alpha * Sample) + ((1 - Alpha) * Previous)
            // We use 256 to represent "1"
            mCurrentEma = (mAlpha * scaledSample + (256 - mAlpha) * mCurrentEma.value_or(0)) >> 8;
        }
    }

    uint64_t getValue() const
    {
        // Shift back to get the real integer value
        return mCurrentEma.value_or(0) >> 8;
    }

private:
    uint32_t mAlpha; // Scale of 256
    std::optional<uint64_t> mCurrentEma{}; // Stored in fixed-point (Value * 256)
};

// Incrementally computes the maximum in a sequence of values.
template<typename T>
using Maximizer = detail::Latcher<std::greater, T>;

// Incrementally computes the minimum in a sequence of values.
template<typename T>
using Minimizer = detail::Latcher<std::less, T>;

} // common
} // mega
