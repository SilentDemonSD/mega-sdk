#include "megaapi_impl.h"

#include <gtest/gtest.h>

using ::mega::HttpStreamingBuffer;

TEST(HttpStreamingBuffer, InitializeLessThanMaxBufferSize)
{
    HttpStreamingBuffer buffer("HttpStreamingBufferTest");

    constexpr size_t bufferSize = HttpStreamingBuffer::HTTP_MAX_BUFFER_SIZE - 1;

    buffer.init(bufferSize);
    EXPECT_EQ(buffer.availableCapacity(), bufferSize);
    EXPECT_EQ(buffer.getMaxBufferSize(), HttpStreamingBuffer::HTTP_MAX_BUFFER_SIZE);
    EXPECT_EQ(buffer.getMaxOutputSize(), HttpStreamingBuffer::HTTP_MAX_OUTPUT_SIZE);
}

TEST(HttpStreamingBuffer, InitializeLargerThanMaxBufferSize)
{
    HttpStreamingBuffer buffer("HttpStreamingBufferTest");

    constexpr size_t bufferSize = HttpStreamingBuffer::HTTP_MAX_BUFFER_SIZE + 1;

    buffer.init(bufferSize);
    EXPECT_EQ(buffer.availableCapacity(), HttpStreamingBuffer::HTTP_MAX_BUFFER_SIZE);
    EXPECT_EQ(buffer.getMaxBufferSize(), HttpStreamingBuffer::HTTP_MAX_BUFFER_SIZE);
    EXPECT_EQ(buffer.getMaxOutputSize(), HttpStreamingBuffer::HTTP_MAX_OUTPUT_SIZE);
}
