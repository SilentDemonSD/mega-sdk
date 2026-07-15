/**
 * @file
 * @brief Integration coverage for node-key validation on the streaming download path.
 */

#include "SdkTest_test.h"

#include <gtest/gtest.h>

#include <memory>

class SdkTestStreamingNodeKey: public SdkTest
{};

/**
 * @brief SdkTestStreamingNodeKey.StreamingForeignNodeWithInvalidKeyFailsWithEKey
 *
 * Streams a foreign node whose key is not a full file key. A regular owned node
 * download waits for the node key to be applied, but the foreign streaming path has no
 * such deferral: it must fail the transfer cleanly with API_EKEY instead of building a
 * zero-key cipher or reading the CTR IV past the short key buffer.
 */
TEST_F(SdkTestStreamingNodeKey, StreamingForeignNodeWithInvalidKeyFailsWithEKey)
{
    LOG_info << "___TEST StreamingForeignNodeWithInvalidKeyFailsWithEKey___";
    ASSERT_NO_FATAL_FAILURE(getAccountsForTest(1));

    // A foreign file node whose base64 key decodes to fewer than FILENODEKEYLENGTH (32)
    // bytes: keyApplied() would be false for an owned node, and the raw key is too short to
    // hold the IV read at byte offset SymmCipher::KEYLENGTH.
    constexpr int64_t fileSize = 1024;
    std::unique_ptr<MegaNode> foreignNode(
        megaApi[0]->createForeignFileNode(1234 /*handle*/,
                                          "AAAA" /*key (base64) -> 3 decoded bytes*/,
                                          "foreign.bin" /*name*/,
                                          fileSize,
                                          0 /*mtime*/,
                                          nullptr /*fingerprintCrc*/,
                                          INVALID_HANDLE /*parentHandle*/,
                                          nullptr /*privateAuth*/,
                                          nullptr /*publicAuth*/,
                                          nullptr /*chatAuth*/));
    ASSERT_NE(foreignNode, nullptr);
    ASSERT_TRUE(foreignNode->isForeign());
    ASSERT_NE(foreignNode->getNodeKey()->size(), static_cast<size_t>(FILENODEKEYLENGTH));

    // The key guard runs before any network request, so no download actually starts.
    TransferTracker tracker(megaApi[0].get());
    megaApi[0]->setStreamingMinimumRate(0);
    megaApi[0]->startStreaming(foreignNode.get(), 0 /*startPos*/, fileSize /*size*/, &tracker);

    ASSERT_EQ(tracker.waitForResult(), API_EKEY);
}
