/**
 * @file SqliteLexicographicListing_test.cpp
 * @brief Offline correctness tests for DBTableNodes::listChildNodesLexicographically.
 *
 * Drives the DB-layer listing directly (no account/network) to pin the S3-key ordering and
 * seek/continuation contract; SdkTestGetChildrenLexicographic covers it end-to-end. Effective
 * S3 key: folder = name + '/', file = name; byte-wise order, tiebroken by nodehandle.
 */

#include "utils.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <mega/db/sqlite.h>
#include <mega/megaapp.h>
#include <mega/megaclient.h>
#include <mega/nodemanager.h>

#include <filesystem>
#include <map>
#include <mega.h>
#include <optional>
#include <stdfs.h>
#include <string>
#include <vector>

#ifdef USE_SQLITE

namespace fs = std::filesystem;
using namespace mega;

namespace
{

// Lower-than-'/'(0x2f) bytes used to exercise the regression boundary:
//   '-' = 0x2d, '.' = 0x2e  →  both sort before a folder's trailing '/'.

class LexiListingTest: public ::testing::Test
{
protected:
    MegaApp mApp;
    NodeManager::MissingParentNodes mMissingParentNodes;
    std::shared_ptr<MegaClient> mClient;
    fs::path mTestDir;

    uint64_t mNextHandle = 1;
    std::shared_ptr<Node> mParent;
    NodeHandle mParentHandle;
    bool mIndexed = false;

    // handle (as8byte) -> effective S3 key, for asserting result order by name.
    std::map<handle, std::string> mKeyByHandle;

    void SetUp() override
    {
        mTestDir = fs::current_path() / "sqlite_lexi_listing_test";
        fs::remove_all(mTestDir); // clear stale WAL files from a crashed run
        fs::create_directories(mTestDir);

        auto* dbAccess = new SqliteDbAccess(LocalPath::fromAbsolutePath(path_u8string(mTestDir)));
        mClient = mt::makeClient(mApp, dbAccess);
        mClient->sid =
            "AWA5YAbtb4JO-y2zWxmKZpSe5-6XM7CTEkA-3Nv7J4byQUpOazdfSC1ZUFlS-kah76gPKUEkTF9g7MeE";
        mClient->opensctable();

        auto root = make(ROOTNODE, "ROOT", nullptr);
        mParent = make(FOLDERNODE, "P", root.get());
        mParentHandle = mParent->nodeHandle();
    }

    void TearDown() override
    {
        mParent.reset();
        mClient.reset();
        fs::remove_all(mTestDir);
    }

    // Low-level node factory mirroring the production insert path.
    std::shared_ptr<Node> make(nodetype_t type, const std::string& name, Node* parent)
    {
        NodeHandle h = NodeHandle().set6byte(mNextHandle++);
        auto node = mt::makeNode(*mClient, type, h, parent);

        static const nameid nameId = AttrMap::string2nameid("n");
        node->attrs.map[nameId] = name;

        if (type == FILENODE)
        {
            node->size = static_cast<m_off_t>(mNextHandle * 512);
            node->ctime = static_cast<m_time_t>(1700000000LL + static_cast<int64_t>(mNextHandle));
            node->mtime = node->ctime;
            node->crc[0] = static_cast<int32_t>(mNextHandle);
            node->isvalid = true;
            node->serializefingerprint(&node->attrs.map['c']);
            node->setfingerprint();
        }

        mClient->mNodeManager.addNode(node,
                                      /*notify=*/false,
                                      /*isFetching=*/true,
                                      mMissingParentNodes);
        mClient->mNodeManager.saveNodeInDb(node.get());
        return node;
    }

    // Seed one child of the test parent and record its effective S3 key.
    NodeHandle seed(nodetype_t type, const std::string& name)
    {
        auto node = make(type, name, mParent.get());
        const std::string key = type == FOLDERNODE ? name + "/" : name;
        mKeyByHandle[node->nodeHandle().as8byte()] = key;
        return node->nodeHandle();
    }

    DBTableNodes* table()
    {
        return dynamic_cast<DBTableNodes*>(mClient->sctable.get());
    }

    std::vector<std::pair<NodeHandle, NodeSerialized>>
        raw(size_t max, std::optional<NodeSearchLexicographicalOffset> off = std::nullopt)
    {
        if (!mIndexed) // build indexes after seeding, as production does post bulk-insert
        {
            if (auto* sa = dynamic_cast<SqliteAccountState*>(mClient->sctable.get()))
                sa->createIndexes(/*enableSearch=*/true, /*enableLexi=*/true);
            mIndexed = true;
        }
        std::vector<std::pair<NodeHandle, NodeSerialized>> out;
        CancelToken ct;
        EXPECT_TRUE(
            table()->listChildNodesLexicographically(mParentHandle.as8byte(), out, ct, max, off));
        return out;
    }

    // Result as effective-S3-key strings (folders carry a trailing '/').
    std::vector<std::string> keys(size_t max,
                                  std::optional<NodeSearchLexicographicalOffset> off = std::nullopt)
    {
        std::vector<std::string> names;
        for (auto& [h, ser]: raw(max, off))
            names.push_back(mKeyByHandle.at(h.as8byte()));
        return names;
    }

    // Result as raw handles, for asserting the nodehandle tiebreak among equal keys.
    std::vector<handle> handles(size_t max,
                                std::optional<NodeSearchLexicographicalOffset> off = std::nullopt)
    {
        std::vector<handle> hs;
        for (auto& [h, ser]: raw(max, off))
            hs.push_back(h.as8byte());
        return hs;
    }

    static NodeSearchLexicographicalOffset afterKey(std::string name)
    {
        NodeSearchLexicographicalOffset o;
        o.mLastName = std::move(name);
        return o; // mLastHandle unset -> strictly after the key
    }

    static NodeSearchLexicographicalOffset atKey(std::string name)
    {
        NodeSearchLexicographicalOffset o;
        o.mLastName = std::move(name);
        o.mLastHandle = 0; // engaged 0 -> inclusive of the node whose key == name
        return o;
    }

    static NodeSearchLexicographicalOffset afterNode(std::string name, NodeHandle h)
    {
        NodeSearchLexicographicalOffset o;
        o.mLastName = std::move(name);
        o.mLastHandle = h.as8byte();
        return o;
    }
};

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;

// A1, A3, A4: full ordering by effective S3 key, including case, the bare-name-
// vs-folder-key pair (a < a/), and a folder sorting before a higher-byte sibling
// (a/ < a1).
TEST_F(LexiListingTest, OrdersFoldersAndFilesByS3Key)
{
    seed(FILENODE, "a");
    seed(FOLDERNODE, "a");
    seed(FILENODE, "a1");
    seed(FILENODE, "a2");
    seed(FILENODE, "a11");
    seed(FILENODE, "A1");
    seed(FILENODE, "A2");
    seed(FILENODE, "B");
    seed(FILENODE, "b");
    seed(FOLDERNODE, "b");
    seed(FILENODE, "ab");
    seed(FILENODE, "aB");

    EXPECT_THAT(keys(0),
                ElementsAre("A1", "A2", "B", "a", "a/", "a1", "a11", "a2", "aB", "ab", "b", "b/"));
}

// A2 (the regression): a folder sorts AFTER prefix-sibling files whose next byte
// is below '/'. With raw-name ordering the folder "data" would precede them.
TEST_F(LexiListingTest, FolderSortsAfterLowBytePrefixSiblings)
{
    seed(FOLDERNODE, "data");
    seed(FILENODE, "data-archive"); // '-' = 0x2d < '/'
    seed(FILENODE, "data.bak"); // '.' = 0x2e < '/'
    seed(FILENODE, "data1"); // '1' = 0x31 > '/'

    EXPECT_THAT(keys(0), ElementsAre("data-archive", "data.bak", "data/", "data1"));
}

// A5: nodes sharing an identical S3 key are tiebroken by ascending nodehandle.
TEST_F(LexiListingTest, DuplicateKeysTiebreakByHandle)
{
    const auto dir1 = seed(FOLDERNODE, "dir");
    const auto dir2 = seed(FOLDERNODE, "dir"); // same key "dir/"
    const auto dup1 = seed(FILENODE, "dup");
    const auto dup2 = seed(FILENODE, "dup"); // same key "dup"

    // "dir/" < "dup" (3rd byte 'r' shared, then '/' vs ... actually 'i'<'u' at idx 1).
    EXPECT_THAT(handles(0),
                ElementsAre(dir1.as8byte(), dir2.as8byte(), dup1.as8byte(), dup2.as8byte()));
}

// B1: maxElements caps the page.
TEST_F(LexiListingTest, LimitHonored)
{
    seed(FILENODE, "A1");
    seed(FILENODE, "A2");
    seed(FILENODE, "B");
    seed(FILENODE, "a");

    EXPECT_THAT(keys(3), ElementsAre("A1", "A2", "B"));
}

// B2: an offset of mLastName alone seeks strictly past that key.
TEST_F(LexiListingTest, OffsetByKeyIsExclusive)
{
    seed(FILENODE, "a");
    seed(FOLDERNODE, "a");
    seed(FILENODE, "a1");
    seed(FILENODE, "a2");

    // "a" excludes the file "a", lands on the folder key "a/".
    EXPECT_THAT(keys(0, afterKey("a")), ElementsAre("a/", "a1", "a2"));
    // "a/" excludes the folder, lands on "a1".
    EXPECT_THAT(keys(0, afterKey("a/")), ElementsAre("a1", "a2"));
}

// B4: mLastHandle = 0 makes the bound inclusive of the node whose key == mLastName.
TEST_F(LexiListingTest, OffsetHandleZeroIsInclusive)
{
    seed(FILENODE, "a");
    seed(FILENODE, "b");

    EXPECT_THAT(keys(0, atKey("a")), ElementsAre("a", "b")); // inclusive
    EXPECT_THAT(keys(0, afterKey("a")), ElementsAre("b")); // exclusive, for contrast
}

// B3: among equal-key duplicates, mLastHandle resumes strictly after that node.
TEST_F(LexiListingTest, ResumeAfterSpecificDuplicate)
{
    const auto dup1 = seed(FILENODE, "dup");
    const auto dup2 = seed(FILENODE, "dup");
    const auto z = seed(FILENODE, "z");

    EXPECT_THAT(handles(0, afterNode("dup", dup1)), ElementsAre(dup2.as8byte(), z.as8byte()));
}

// B5, B6: take a page that ends between a low-byte sibling and the folder, then
// resume from the last node by (key, handle) — the tail must follow with no gap
// or duplicate across the folder-key boundary.
TEST_F(LexiListingTest, ResumeAcrossFolderBoundary)
{
    seed(FOLDERNODE, "data");
    seed(FILENODE, "data-archive");
    const auto bak = seed(FILENODE, "data.bak");
    seed(FILENODE, "data1");

    const auto page1 = keys(2);
    EXPECT_THAT(page1, ElementsAre("data-archive", "data.bak"));

    // Resume after the last node of page1 (the file "data.bak").
    EXPECT_THAT(keys(0, afterNode("data.bak", bak)), ElementsAre("data/", "data1"));
}

// Node type is ignored as a seek dimension — it's folded into the S3 key. The internal offset has
// no type field to honour, so that's a compile-time guarantee rather than a runtime test.

// Empty parent: a folder with no children lists nothing.
TEST_F(LexiListingTest, EmptyParentReturnsNothing)
{
    EXPECT_THAT(keys(0), ::testing::IsEmpty());
}

// maxElements larger than the result set returns everything (no over-read, no padding).
TEST_F(LexiListingTest, MaxElementsBeyondCountReturnsAll)
{
    seed(FILENODE, "a");
    seed(FILENODE, "b");
    seed(FOLDERNODE, "c");

    EXPECT_THAT(keys(100), ElementsAre("a", "b", "c/"));
}

// An offset seeked past the last key returns nothing.
TEST_F(LexiListingTest, OffsetPastEndReturnsNothing)
{
    seed(FILENODE, "a");
    seed(FILENODE, "b");

    EXPECT_THAT(keys(0, afterKey("z")), ::testing::IsEmpty());
}

} // namespace

#endif // USE_SQLITE
