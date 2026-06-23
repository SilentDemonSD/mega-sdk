/**
 * (c) 2026 by Mega Limited, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#include "DefaultedDbTable.h"
#include "utils.h"

#include <gtest/gtest.h>
#include <mega/command.h>
#include <mega/megaapp.h>
#include <mega/megaclient.h>
#include <mega/node.h>
#include <mega/treeproc.h>
#include <mega/types.h>

#include <memory>
#include <string>

using namespace mega;

namespace
{

// A raw wire-form key as kept for an undecryptable / foreign node. The only
// property that matters is that its length matches neither a file key
// (FILENODEKEYLENGTH = 32) nor a folder key (FOLDERNODEKEYLENGTH = 16), so
// keyApplied() is always false and it must not be used as a fixed-length key.
const std::string kRawOverlongKey = "share:" + std::string(FILENODEKEYLENGTH, 'x');

// CommandNodeKeyUpdate builds its request into the protected jsonWriter while
// running its constructor. This thin subclass exposes that built string so a test
// can assert whether a node was actually encoded into the "nk" array.
struct ProbeNodeKeyUpdate: CommandNodeKeyUpdate
{
    ProbeNodeKeyUpdate(MegaClient* c, handle_vector* v):
        CommandNodeKeyUpdate(c, v)
    {}

    const std::string& builtJson() const
    {
        return jsonWriter.getstring();
    }
};

class NodeKeyValidationTest: public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Pure in-memory node manager backed by a stub DB table.
        auto* table = new mt::DefaultedDbTable(mRng);
        client->sctable.reset(table);
        client->mNodeManager.setTable(table);

        // Add a root so the node manager can host child nodes.
        addNode(ROOTNODE, 1);
    }

    void TearDown() override
    {
        client.reset();
    }

    std::shared_ptr<Node> addNode(const nodetype_t type, const handle h)
    {
        auto& nodeRef = mt::makeNode(*client, type, NodeHandle().set6byte(h), nullptr);
        std::shared_ptr<Node> node(&nodeRef);

        NodeManager::MissingParentNodes missingParentNodes;
        client->mNodeManager.addNode(node, false, false, missingParentNodes);

        return node;
    }

    mega::PrnGen mRng;
    mega::MegaApp app;
    std::shared_ptr<mega::MegaClient> client = mt::makeClient(app);
};

TEST_F(NodeKeyValidationTest, KeyAppliedReflectsKeyLength)
{
    auto node = addNode(FILENODE, 10);

    // Normal node with a properly sized key.
    ASSERT_TRUE(node->keyApplied());

    // The raw wire-form key.
    node->setKey(kRawOverlongKey);
    ASSERT_GT(kRawOverlongKey.size(), static_cast<size_t>(FILENODEKEYLENGTH));
    EXPECT_FALSE(node->keyApplied());
}

TEST_F(NodeKeyValidationTest, ForeignKeyRewriteSkipsNodeWithUnappliedKey)
{
    auto node = addNode(FILENODE, 11);
    node->foreignkey = true;
    node->setKey(kRawOverlongKey);
    ASSERT_FALSE(node->keyApplied());

    client->nodekeyrewrite.clear();

    TreeProcForeignKeys proc;
    proc.proc(client.get(), node);

    EXPECT_TRUE(client->nodekeyrewrite.empty());
    EXPECT_TRUE(node->foreignkey);
}

// Verify the normal behavior for a file node.
TEST_F(NodeKeyValidationTest, ForeignKeyRewriteCollectsNodeWithAppliedKey)
{
    auto node = addNode(FILENODE, 12);
    node->foreignkey = true;
    ASSERT_TRUE(node->keyApplied());

    client->nodekeyrewrite.clear();

    TreeProcForeignKeys proc;
    proc.proc(client.get(), node);

    ASSERT_EQ(client->nodekeyrewrite.size(), 1u);
    EXPECT_EQ(client->nodekeyrewrite.front(), node->nodehandle);
    EXPECT_FALSE(node->foreignkey);
}

TEST_F(NodeKeyValidationTest, ForeignKeyRewriteSkipsFolderWithUnappliedKey)
{
    auto node = addNode(FOLDERNODE, 13);
    node->foreignkey = true;
    node->setKey(kRawOverlongKey);
    ASSERT_FALSE(node->keyApplied());

    client->nodekeyrewrite.clear();

    TreeProcForeignKeys proc;
    proc.proc(client.get(), node);

    EXPECT_TRUE(client->nodekeyrewrite.empty());
    EXPECT_TRUE(node->foreignkey);
}

// Verify the normal behavior for a folder node.
TEST_F(NodeKeyValidationTest, ForeignKeyRewriteCollectsFolderWithAppliedKey)
{
    auto node = addNode(FOLDERNODE, 14);
    node->foreignkey = true;
    ASSERT_TRUE(node->keyApplied());

    client->nodekeyrewrite.clear();

    TreeProcForeignKeys proc;
    proc.proc(client.get(), node);

    ASSERT_EQ(client->nodekeyrewrite.size(), 1u);
    EXPECT_EQ(client->nodekeyrewrite.front(), node->nodehandle);
    EXPECT_FALSE(node->foreignkey);
}

// Use a helper subclass to inspect the request that CommandNodeKeyUpdate builds:
// an applied-key node is encoded, an unapplied-key node is skipped.
TEST_F(NodeKeyValidationTest, CommandNodeKeyUpdateEncodesAppliedKeyAndSkipsUnapplied)
{
    const std::string masterKey(SymmCipher::KEYLENGTH, 'K');
    client->key.setkey(reinterpret_cast<const byte*>(masterKey.data()));

    auto applied = addNode(FILENODE, 30);
    applied->setKey(std::string(FILENODEKEYLENGTH / 2, 'A') +
                    std::string(FILENODEKEYLENGTH / 2, 'B'));
    ASSERT_TRUE(applied->keyApplied());

    auto unapplied = addNode(FILENODE, 31);
    unapplied->setKey(kRawOverlongKey);
    ASSERT_FALSE(unapplied->keyApplied());

    handle_vector appliedHandles{applied->nodehandle};
    handle_vector unappliedHandles{unapplied->nodehandle};
    handle_vector empty;

    const std::string appliedJson = ProbeNodeKeyUpdate(client.get(), &appliedHandles).builtJson();
    const std::string skippedJson = ProbeNodeKeyUpdate(client.get(), &unappliedHandles).builtJson();
    const std::string emptyJson = ProbeNodeKeyUpdate(client.get(), &empty).builtJson();

    // Applied key node is encoded.
    EXPECT_NE(appliedJson, emptyJson);

    // Unapplied key node is skipped.
    EXPECT_EQ(skippedJson, emptyJson);
}

} // anonymous namespace
