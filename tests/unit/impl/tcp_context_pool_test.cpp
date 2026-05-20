#include "megaapi_impl.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace mega;
using namespace testing;

namespace
{

// Mock class for MegaTCPContext to avoid dependencies
class MockMegaTCPContext: public MegaTCPContext
{
public:
    MockMegaTCPContext() = default;
    virtual ~MockMegaTCPContext() = default;
};

// Test fixture for TcpConnectionPool
class TcpContextPoolTest: public Test
{
protected:
    void SetUp() override
    {
        pool = std::make_unique<TcpContextPool>();
    }

    void TearDown() override
    {
        pool.reset();
    }

    std::unique_ptr<TcpContextPool> pool;
};

TEST_F(TcpContextPoolTest, InitiallyEmpty)
{
    EXPECT_EQ(pool->size(), 0);
    EXPECT_EQ(pool->back(), nullptr);

    auto copyResult = pool->copy();
    EXPECT_TRUE(copyResult.empty());
}

TEST_F(TcpContextPoolTest, AddSingleContext)
{
    auto context = std::make_shared<MockMegaTCPContext>();
    auto rawPtr = context.get();

    EXPECT_TRUE(pool->add(context));

    EXPECT_EQ(pool->size(), 1);
    EXPECT_EQ(pool->back(), rawPtr);

    auto copyResult = pool->copy();
    EXPECT_EQ(copyResult.size(), 1);
    EXPECT_EQ(copyResult[0], rawPtr);
}

TEST_F(TcpContextPoolTest, AddMultipleContexts)
{
    auto context1 = std::make_shared<MockMegaTCPContext>();
    auto context2 = std::make_shared<MockMegaTCPContext>();
    auto context3 = std::make_shared<MockMegaTCPContext>();

    auto rawPtr1 = context1.get();
    auto rawPtr2 = context2.get();
    auto rawPtr3 = context3.get();

    EXPECT_TRUE(pool->add(context1));
    EXPECT_TRUE(pool->add(context2));
    EXPECT_TRUE(pool->add(context3));

    EXPECT_EQ(pool->size(), 3);
    EXPECT_EQ(pool->back(), rawPtr3); // Last added should be at back

    auto copyResult = pool->copy();
    EXPECT_EQ(copyResult.size(), 3);
    EXPECT_EQ(copyResult[0], rawPtr1);
    EXPECT_EQ(copyResult[1], rawPtr2);
    EXPECT_EQ(copyResult[2], rawPtr3);
}

TEST_F(TcpContextPoolTest, AddDuplicateContext)
{
    auto context = std::make_shared<MockMegaTCPContext>();
    auto rawPtr = context.get();

    // Add the same context multiple times
    EXPECT_TRUE(pool->add(context));
    EXPECT_FALSE(pool->add(context)); // Should fail on duplicate

    // Should still only have one entry
    EXPECT_EQ(pool->size(), 1);
    EXPECT_EQ(pool->back(), rawPtr);
}

TEST_F(TcpContextPoolTest, AddNullptrShouldFail)
{
    // Try to add nullptr
    EXPECT_FALSE(pool->add(nullptr));

    // Pool should remain empty
    EXPECT_EQ(pool->size(), 0);
    EXPECT_EQ(pool->back(), nullptr);

    auto copyResult = pool->copy();
    EXPECT_TRUE(copyResult.empty());
}

TEST_F(TcpContextPoolTest, ReleaseExistingContext)
{
    auto context1 = std::make_shared<MockMegaTCPContext>();
    auto context2 = std::make_shared<MockMegaTCPContext>();

    auto rawPtr1 = context1.get();
    auto rawPtr2 = context2.get();

    EXPECT_TRUE(pool->add(context1));
    EXPECT_TRUE(pool->add(context2));

    EXPECT_EQ(pool->size(), 2);

    // Release the first context
    auto released = pool->release(rawPtr1);

    EXPECT_NE(released, nullptr);
    EXPECT_EQ(released.get(), rawPtr1);
    EXPECT_EQ(pool->size(), 1);
    EXPECT_EQ(pool->back(), rawPtr2);

    auto copyResult = pool->copy();
    EXPECT_EQ(copyResult.size(), 1);
    EXPECT_EQ(copyResult[0], rawPtr2);
}

TEST_F(TcpContextPoolTest, ReleaseNonExistentContext)
{
    auto context1 = std::make_shared<MockMegaTCPContext>();
    auto context2 = std::make_shared<MockMegaTCPContext>();
    auto context3 = std::make_shared<MockMegaTCPContext>();

    auto rawPtr3 = context3.get(); // This one won't be added to pool

    EXPECT_TRUE(pool->add(context1));
    EXPECT_TRUE(pool->add(context2));

    EXPECT_EQ(pool->size(), 2);

    // Try to release a context that wasn't added
    auto released = pool->release(rawPtr3);

    EXPECT_EQ(released, nullptr);
    EXPECT_EQ(pool->size(), 2); // Size should remain unchanged
}

TEST_F(TcpContextPoolTest, ReleaseFromEmptyPool)
{
    auto context = std::make_shared<MockMegaTCPContext>();
    auto rawPtr = context.get();

    // Try to release from empty pool
    auto released = pool->release(rawPtr);

    EXPECT_EQ(released, nullptr);
    EXPECT_EQ(pool->size(), 0);
}

TEST_F(TcpContextPoolTest, ReleaseAllContexts)
{
    auto context1 = std::make_shared<MockMegaTCPContext>();
    auto context2 = std::make_shared<MockMegaTCPContext>();
    auto context3 = std::make_shared<MockMegaTCPContext>();

    auto rawPtr1 = context1.get();
    auto rawPtr2 = context2.get();
    auto rawPtr3 = context3.get();

    EXPECT_TRUE(pool->add(context1));
    EXPECT_TRUE(pool->add(context2));
    EXPECT_TRUE(pool->add(context3));

    EXPECT_EQ(pool->size(), 3);

    // Release all contexts
    auto released1 = pool->release(rawPtr2); // Release middle one first
    auto released2 = pool->release(rawPtr1); // Release first one
    auto released3 = pool->release(rawPtr3); // Release last one

    EXPECT_NE(released1, nullptr);
    EXPECT_NE(released2, nullptr);
    EXPECT_NE(released3, nullptr);

    EXPECT_EQ(released1.get(), rawPtr2);
    EXPECT_EQ(released2.get(), rawPtr1);
    EXPECT_EQ(released3.get(), rawPtr3);

    EXPECT_EQ(pool->size(), 0);
    EXPECT_EQ(pool->back(), nullptr);

    auto copy_result = pool->copy();
    EXPECT_TRUE(copy_result.empty());
}

TEST_F(TcpContextPoolTest, CopyReturnsCorrectOrder)
{
    auto context1 = std::make_shared<MockMegaTCPContext>();
    auto context2 = std::make_shared<MockMegaTCPContext>();
    auto context3 = std::make_shared<MockMegaTCPContext>();

    auto rawPtr1 = context1.get();
    auto rawPtr2 = context2.get();
    auto rawPtr3 = context3.get();

    EXPECT_TRUE(pool->add(context1));
    EXPECT_TRUE(pool->add(context2));
    EXPECT_TRUE(pool->add(context3));

    auto copyResult = pool->copy();

    EXPECT_EQ(copyResult.size(), 3);
    EXPECT_EQ(copyResult[0], rawPtr1); // First added should be first in copy
    EXPECT_EQ(copyResult[1], rawPtr2);
    EXPECT_EQ(copyResult[2], rawPtr3); // Last added should be last in copy
}

TEST_F(TcpContextPoolTest, BackReturnsLastAdded)
{
    EXPECT_EQ(pool->back(), nullptr); // Empty pool

    auto context1 = std::make_shared<MockMegaTCPContext>();
    auto context2 = std::make_shared<MockMegaTCPContext>();
    auto context3 = std::make_shared<MockMegaTCPContext>();

    auto rawPtr1 = context1.get();
    auto rawPtr2 = context2.get();
    auto rawPtr3 = context3.get();

    EXPECT_TRUE(pool->add(context1));
    EXPECT_EQ(pool->back(), rawPtr1);

    EXPECT_TRUE(pool->add(context2));
    EXPECT_EQ(pool->back(), rawPtr2);

    EXPECT_TRUE(pool->add(context3));
    EXPECT_EQ(pool->back(), rawPtr3);
}

TEST_F(TcpContextPoolTest, AddReleaseAddSequence)
{
    auto context1 = std::make_shared<MockMegaTCPContext>();
    auto context2 = std::make_shared<MockMegaTCPContext>();

    auto rawPtr1 = context1.get();
    auto rawPtr2 = context2.get();

    // Add first context
    EXPECT_TRUE(pool->add(context1));
    EXPECT_EQ(pool->size(), 1);
    EXPECT_EQ(pool->back(), rawPtr1);

    // Release it
    auto released = pool->release(rawPtr1);
    EXPECT_NE(released, nullptr);
    EXPECT_EQ(pool->size(), 0);
    EXPECT_EQ(pool->back(), nullptr);

    // Add second context
    EXPECT_TRUE(pool->add(context2));
    EXPECT_EQ(pool->size(), 1);
    EXPECT_EQ(pool->back(), rawPtr2);

    // Verify copy works correctly
    auto copy_result = pool->copy();
    EXPECT_EQ(copy_result.size(), 1);
    EXPECT_EQ(copy_result[0], rawPtr2);
}

TEST_F(TcpContextPoolTest, MemoryManagement)
{
    // Test that contexts are properly managed with shared_ptr
    std::weak_ptr<MockMegaTCPContext> weak_ref;
    MockMegaTCPContext* rawPtr = nullptr;

    {
        auto context = std::make_shared<MockMegaTCPContext>();
        weak_ref = context;
        rawPtr = context.get();

        EXPECT_TRUE(pool->add(context));

        // Context should still be alive (held by pool)
        EXPECT_FALSE(weak_ref.expired());
    }

    // Context should still be alive (held by pool)
    EXPECT_FALSE(weak_ref.expired());

    // Release the context
    auto released = pool->release(rawPtr);
    EXPECT_NE(released, nullptr);

    // Context should still be alive (held by released shared_ptr)
    EXPECT_FALSE(weak_ref.expired());

    // Release the released reference
    released.reset();

    // Now context should be destroyed
    EXPECT_TRUE(weak_ref.expired());
}

}
