#include "megaapi_impl.h"

#include <gtest/gtest.h>

using ::mega::HttpStreamingBuffer;

TEST(HttpStreamingBuffer, InitializeLessThanMaxBufferSize)
{
    HttpStreamingBuffer buffer("HttpStreamingBufferTest");

    constexpr size_t bufferSize = 1 * 1024 * 1024;
    ASSERT_LE(bufferSize, HttpStreamingBuffer::MAX_BUFFER_SIZE);

    buffer.init(bufferSize);
    EXPECT_EQ(buffer.availableCapacity(), bufferSize);
    EXPECT_EQ(buffer.getMaxBufferSize(), HttpStreamingBuffer::MAX_BUFFER_SIZE);
    EXPECT_EQ(buffer.getMaxOutputSize(), HttpStreamingBuffer::MAX_OUTPUT_SIZE);
}

TEST(HttpStreamingBuffer, InitializeLargerThanMaxBufferSize)
{
    HttpStreamingBuffer buffer("HttpStreamingBufferTest");

    constexpr size_t bufferSize = 2 * 1024 * 1024 + 1;
    ASSERT_GE(bufferSize, HttpStreamingBuffer::MAX_BUFFER_SIZE);

    buffer.init(bufferSize);
    EXPECT_EQ(buffer.availableCapacity(), HttpStreamingBuffer::MAX_BUFFER_SIZE);
    EXPECT_EQ(buffer.getMaxBufferSize(), HttpStreamingBuffer::MAX_BUFFER_SIZE);
    EXPECT_EQ(buffer.getMaxOutputSize(), HttpStreamingBuffer::MAX_OUTPUT_SIZE);
}