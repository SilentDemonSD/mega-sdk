/**
 * @file SqliteDateSectionsPerf_test.cpp
 * @brief Performance tests for SDK-6171 date-section + timestamp-anchor work.
 *
 * Separate from SqliteNodesPerf_test.cpp because that fixture packs every node
 * into one date bucket, so it can't exercise the date-granularity code paths.
 *
 * Dataset (built in SetUp): 3 roots (ROOT/VAULT/RUBBISH), 10 top + 100
 * sub-folders, 100,000 files with mtime spread across 730 days from 2023-11-14,
 * plus 10,000 rubbish files (same mime mix, to test the "index match but root
 * filter rejects" path). The spread yields 730 Day / 24 Month / 2-3 Year
 * buckets.
 *
 * SetUp takes ~30s. All tests are DISABLED_*; CI skips them. Run with:
 *   ./build-sdk-dev-unix/tests/unit/test_unit \
 *       --gtest_also_run_disabled_tests \
 *       --gtest_filter='*SqliteDateSections*:*ListAllByAnchor*:*CacheEffects*'
 *
 * Numbers are logged, not asserted — compare against
 * tests/unit/SqliteNodesPerf_baseline.md on the SAME machine; cross-machine
 * comparison is meaningless.
 *
 * This file holds four suites sharing DateSpreadFixtureBase; the
 * one-suite-per-file rule (tests/README.md) is relaxed to avoid paying the
 * ~30s populate cost 4×.
 */

#include "utils.h"

#include <gtest/gtest.h>
#include <mega/db/sqlite.h>
#include <mega/megaapp.h>
#include <mega/megaclient.h>
#include <mega/nodemanager.h>

#include <chrono>
#include <filesystem>
#include <mega.h>
#include <optional>
#include <sstream>
#include <stdfs.h>
#include <string>
#include <vector>

#ifdef USE_SQLITE

namespace fs = std::filesystem;

using namespace mega;

namespace
{

// ─── Shared iteration / timing helpers (duplicated from SqliteNodesPerf_test ─
// to avoid a cross-file shared header for just two consumers; ~15 lines) ─────

constexpr int SIMPLE_ITERS = 1000;
constexpr int COMPLEX_ITERS = 100;

template<typename Fn>
long long measureUs(int iters, Fn&& fn)
{
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i)
        fn();
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
}

// ─── Dataset dimensions (large fixture) ─────────────────────────────────────
// Tunable as constexpr so a future regression-bisect can scale down quickly.

constexpr int64_t kBaseEpoch = 1700000000LL; // 2023-11-14 22:13:20 UTC
// numDays is intentionally coprime with the mime-distribution period (10) so
// (day % numDays, idx % 10) walks every combination — otherwise every day
// would have a fixed mime and `ALL_VISUAL_MEDIA` would cover only ~60% of
// days, defeating the perf test's goal of many distinct buckets.
constexpr int kLargeNumDays = 731; // 2 years + 1 day; coprime w/ 10 (731 = 17*43)
constexpr int kLargeNumFiles = 100'000;
constexpr int kLargeNumTopFolders = 10;
constexpr int kLargeNumSubPerTop = 10; // → 100 sub-folders
constexpr int kIntraDaySeconds = 86400 / 137; // ~630 s; spreads ~137 files / day

// ─── Smaller dataset for the cache-effects fixture ──────────────────────────
// Phase 4 needs to rebuild SqliteAccountState per iter to measure cold
// prepare cost. At 100K nodes a rebuild takes ~30s; at 5K it's < 0.5s,
// making 30-50 iters practical.

constexpr int kSmallNumDays = 30;
constexpr int kSmallNumFiles = 5'000;
constexpr int kSmallNumTopFolders = 5;
constexpr int kSmallNumSubPerTop = 5;
// ~2618 s per file. The intra-day step times the round count overflows a day, so
// late-round files spill a few days forward — actual bucket count slightly exceeds
// kSmallNumDays. Harmless here (Phase 4 asserts no bucket count); don't add one
// without snapping mtimes to day boundaries first.
constexpr int kSmallIntraDaySeconds = 86400 / 33;

// ─── Common dataset-build helpers ───────────────────────────────────────────

struct DateSpreadParams
{
    int numDays;
    int numFiles;
    int numTopFolders;
    int numSubPerTop;
    int intraDaySeconds;
    int rubbishRatioDenom; // numFiles / rubbishRatioDenom rubbish files
};

constexpr DateSpreadParams kLargeParams{kLargeNumDays,
                                        kLargeNumFiles,
                                        kLargeNumTopFolders,
                                        kLargeNumSubPerTop,
                                        kIntraDaySeconds,
                                        10};

constexpr DateSpreadParams kSmallParams{kSmallNumDays,
                                        kSmallNumFiles,
                                        kSmallNumTopFolders,
                                        kSmallNumSubPerTop,
                                        kSmallIntraDaySeconds,
                                        10};

// ─── Common fixture base ────────────────────────────────────────────────────
// Subclasses set up `kParams` to pick a dataset scale, then SetUp() handles
// the boilerplate of opening the DB and populating it.

class DateSpreadFixtureBase: public ::testing::Test
{
protected:
    mega::MegaApp mApp;
    NodeManager::MissingParentNodes mMissingParentNodes;
    std::shared_ptr<MegaClient> mClient;
    fs::path mTestDir;

    uint64_t mNextHandle = 1;

    NodeHandle mRootHandle;
    std::vector<NodeHandle> mTopFolderHandles;
    std::vector<NodeHandle> mSubFolderHandles;
    std::vector<NodeHandle> mFileHandles;

    // Bucket-bound tracking — populated during populateDB so anchor tests
    // can pick "first bucket" / "mid bucket" / "last bucket" mtimes.
    int64_t mFirstBucketStartSec = 0;
    int64_t mLastBucketStartSec = 0;
    int64_t mMidBucketStartSec = 0;

    virtual const DateSpreadParams& params() const = 0;
    virtual const char* fixtureName() const = 0;

    void SetUp() override
    {
        mTestDir = fs::current_path() / fixtureName();
        fs::remove_all(mTestDir);
        fs::create_directories(mTestDir);

        auto* dbAccess = new SqliteDbAccess(LocalPath::fromAbsolutePath(path_u8string(mTestDir)));
        mClient = mt::makeClient(mApp, dbAccess);
        mClient->sid =
            "AWA5YAbtb4JO-y2zWxmKZpSe5-6XM7CTEkA-3Nv7J4byQUpOazdfSC1ZUFlS-kah76gPKUEkTF9g7MeE";
        mClient->opensctable();

        populateDB();

        if (auto* sa = dynamic_cast<SqliteAccountState*>(mClient->sctable.get()))
            sa->createIndexes(/*enableSearch=*/true, /*enableLexi=*/true);
    }

    void TearDown() override
    {
        mClient.reset();
        fs::remove_all(mTestDir);
    }

    // ── Node factory (trimmed copy of SqliteNodesPerfTest's; no fav/label) ──
    std::shared_ptr<Node> addNode(nodetype_t nodeType,
                                  std::shared_ptr<Node> parent,
                                  const std::string& name,
                                  m_time_t mtime = 0)
    {
        NodeHandle handle = NodeHandle().set6byte(mNextHandle++);
        Node& nodeRef = mt::makeNode(*mClient, nodeType, handle, parent.get());
        auto node = std::shared_ptr<Node>(&nodeRef);

        static const nameid nameId = AttrMap::string2nameid("n");
        node->attrs.map[nameId] = name;

        if (nodeType == FILENODE)
        {
            node->size = static_cast<m_off_t>(mNextHandle * 512);
            node->ctime = static_cast<m_time_t>(kBaseEpoch + static_cast<int64_t>(mNextHandle));
            node->mtime = mtime;
            node->crc[0] = static_cast<int32_t>(mNextHandle);
            node->crc[1] = static_cast<int32_t>(mNextHandle >> 8u);
            node->crc[2] = static_cast<int32_t>(mNextHandle >> 16u);
            node->crc[3] = static_cast<int32_t>(mNextHandle >> 24u);
            node->isvalid = true;
            node->serializefingerprint(&node->attrs.map['c']);
            node->setfingerprint();

            NodeCounter nc;
            nc.files = 1;
            nc.storage = node->size;
            node->setCounter(nc);
        }

        mClient->mNodeManager.addNode(node,
                                      /*notify=*/false,
                                      /*isFetching=*/true,
                                      mMissingParentNodes);
        mClient->mNodeManager.saveNodeInDb(node.get());
        return node;
    }

    void populateDB()
    {
        const auto& p = params();

        auto rootNode = addNode(ROOTNODE, nullptr, "ROOT");
        mRootHandle = rootNode->nodeHandle();
        addNode(VAULTNODE, nullptr, "VAULT");
        auto rubbishNode = addNode(RUBBISHNODE, nullptr, "RUBBISH");

        std::vector<std::shared_ptr<Node>> subFolders;
        subFolders.reserve(static_cast<size_t>(p.numTopFolders * p.numSubPerTop));

        for (int i = 0; i < p.numTopFolders; ++i)
        {
            auto topFolder = addNode(FOLDERNODE, rootNode, "Folder_" + std::to_string(i));
            mTopFolderHandles.push_back(topFolder->nodeHandle());

            for (int j = 0; j < p.numSubPerTop; ++j)
            {
                auto subFolder =
                    addNode(FOLDERNODE,
                            topFolder,
                            "SubFolder_" + std::to_string(i) + "_" + std::to_string(j));
                mSubFolderHandles.push_back(subFolder->nodeHandle());
                subFolders.push_back(subFolder);
            }
        }

        // Spread numFiles uniformly across numDays: dayOffset = idx % numDays
        // (1 file/day/round), and the round number sets the in-day offset.
        // Yields numDays distinct Day buckets, ceil(numFiles/numDays) max each.
        const int filesPerSub = p.numFiles / static_cast<int>(subFolders.size());
        int globalFileIdx = 0;
        for (size_t s = 0; s < subFolders.size(); ++s)
        {
            for (int k = 0; k < filesPerSub; ++k, ++globalFileIdx)
            {
                const int dayOffset = globalFileIdx % p.numDays;
                const int intraDayRound = globalFileIdx / p.numDays;
                const m_time_t mtime = kBaseEpoch + static_cast<m_time_t>(dayOffset) * 86400LL +
                                       static_cast<m_time_t>(intraDayRound) * p.intraDaySeconds;

                // Same MIME mix as SqliteNodesPerf: 50% jpg / 10% mp4 / 40% txt
                const char* ext = (globalFileIdx % 10 < 5)  ? ".jpg" :
                                  (globalFileIdx % 10 == 5) ? ".mp4" :
                                                              ".txt";
                std::string fname = "f_" + std::to_string(s) + "_" + std::to_string(k) + ext;
                auto file = addNode(FILENODE, subFolders[s], fname, mtime);
                mFileHandles.push_back(file->nodeHandle());
            }
        }

        // Day-bucket boundary mtimes for the byTimestampAnchor tests. Snapped
        // to midnight UTC to match production bounds (strftime '%Y-%m-%d' →
        // 00:00:00 UTC); kBaseEpoch is not midnight-aligned by itself.
        constexpr int64_t kSecondsPerDay = 86400LL;
        const int64_t midnightUtc = (kBaseEpoch / kSecondsPerDay) * kSecondsPerDay;
        mFirstBucketStartSec = midnightUtc;
        mLastBucketStartSec = midnightUtc + static_cast<int64_t>(p.numDays - 1) * kSecondsPerDay;
        mMidBucketStartSec = midnightUtc + static_cast<int64_t>(p.numDays / 2) * kSecondsPerDay;

        // Rubbish files: same mime distribution, separate root.
        const int rubbishCount = p.numFiles / p.rubbishRatioDenom;
        for (int k = 0; k < rubbishCount; ++k)
        {
            const int dayOffset = k % p.numDays;
            const m_time_t mtime = kBaseEpoch + static_cast<m_time_t>(dayOffset) * 86400LL +
                                   static_cast<m_time_t>(k / p.numDays) * p.intraDaySeconds;
            const char* ext = (k % 10 < 5) ? ".jpg" : (k % 10 == 5) ? ".mp4" : ".txt";
            std::string fname = "rubbish_" + std::to_string(k) + ext;
            addNode(FILENODE, rubbishNode, fname, mtime);
        }
    }

    DBTableNodes* nodesTable()
    {
        return dynamic_cast<DBTableNodes*>(mClient->sctable.get());
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  Large fixture (100K nodes / 730 days) — used by Phases 1, 2, 3
// ═══════════════════════════════════════════════════════════════════════════

class SqliteDateSectionsPerfFixture: public DateSpreadFixtureBase
{
protected:
    const DateSpreadParams& params() const override
    {
        return kLargeParams;
    }

    const char* fixtureName() const override
    {
        return "sqlite_date_sections_perf";
    }
};

using DISABLED_SqliteDateSectionsPerfTest = SqliteDateSectionsPerfFixture;

// ─── Phase 1: dataset sanity ────────────────────────────────────────────────

TEST_F(DISABLED_SqliteDateSectionsPerfTest, DateSpreadSanity)
{
    // Confirm the fixture produced exactly kLargeNumDays distinct Day buckets,
    // 24 Month buckets (≈ 2 years), and 2-3 Year buckets.
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    DateSectionParams sp;
    sp.mimeType = MIME_TYPE_ALL_VISUAL_MEDIA;
    sp.order = OrderByClause::MTIME_DESC;

    const std::vector<NodeHandle> roots{mRootHandle};

    for (auto gran:
         {DateSectionGranularity::Day, DateSectionGranularity::Month, DateSectionGranularity::Year})
    {
        std::vector<DateSection> sections;
        CancelToken ct;
        sp.granularity = gran;
        ASSERT_TRUE(table->groupAllNodesByDate(sp, roots, sections, ct));

        size_t totalCount = 0;
        for (const auto& s: sections)
            totalCount += static_cast<size_t>(s.mCount);

        const char* name = gran == DateSectionGranularity::Day ?
                               "Day" :
                               (gran == DateSectionGranularity::Month ? "Month" : "Year");

        GTEST_LOG_(INFO) << "Sanity " << name << ": sections=" << sections.size()
                         << ", total nodes counted=" << totalCount;

        // Range assertions, not exact: the bucket count varies with the
        // mimetypeVirtual classifier. We only need "many buckets" (vs the old
        // fixture's "1") to confirm the spread is meaningful.
        if (gran == DateSectionGranularity::Day)
        {
            EXPECT_GE(sections.size(), 700u); // ~all 731 days for coprime numDays
        }
        if (gran == DateSectionGranularity::Month)
        {
            EXPECT_GE(sections.size(), 23u); // 731 days ≈ 24 months
            EXPECT_LE(sections.size(), 26u);
        }
        if (gran == DateSectionGranularity::Year)
        {
            EXPECT_LE(sections.size(), 3u);
            EXPECT_GE(sections.size(), 2u);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Phase 2: groupAllNodesByDate parametric perf
// ═══════════════════════════════════════════════════════════════════════════

struct GroupByDateParam
{
    MimeType_t mimeType;
    DateSectionGranularity granularity;
    int rootCount;
    bool excludeSensitive;
};

class SqliteNodesPerfGroupByDateTest:
    public SqliteDateSectionsPerfFixture,
    public ::testing::WithParamInterface<GroupByDateParam>
{};

static const char* mimeName(MimeType_t m)
{
    switch (m)
    {
        case MIME_TYPE_VIDEO:
            return "VIDEO";
        case MIME_TYPE_ALL_VISUAL_MEDIA:
            return "ALL_VISUAL_MEDIA";
        case MIME_TYPE_ALL_DOCS:
            return "ALL_DOCS";
        default:
            return "OTHER";
    }
}

static const char* granName(DateSectionGranularity g)
{
    switch (g)
    {
        case DateSectionGranularity::Day:
            return "Day";
        case DateSectionGranularity::Month:
            return "Month";
        case DateSectionGranularity::Year:
            return "Year";
    }
    return "?";
}

TEST_P(SqliteNodesPerfGroupByDateTest, DISABLED_Perf)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const auto& p = GetParam();

    DateSectionParams sp;
    sp.mimeType = p.mimeType;
    sp.order = OrderByClause::MTIME_DESC;
    sp.granularity = p.granularity;
    sp.excludeSensitive = p.excludeSensitive;

    std::vector<NodeHandle> roots{mRootHandle};
    for (int i = 1; i < p.rootCount && i < static_cast<int>(mTopFolderHandles.size()); ++i)
        roots.push_back(mTopFolderHandles[static_cast<size_t>(i)]);

    // Warm prepare; the timed loop measures execution only.
    {
        std::vector<DateSection> warm;
        CancelToken ct;
        table->groupAllNodesByDate(sp, roots, warm, ct);
    }

    size_t sectionCount = 0;
    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<DateSection> sections;
                                       CancelToken ct;
                                       table->groupAllNodesByDate(sp, roots, sections, ct);
                                       sectionCount = sections.size();
                                   });

    GTEST_LOG_(INFO) << "groupAllNodesByDate [mime=" << mimeName(p.mimeType)
                     << ", gran=" << granName(p.granularity) << ", roots=" << roots.size()
                     << ", excludeSens=" << (p.excludeSensitive ? "1" : "0")
                     << ", sections=" << sectionCount << "]: " << COMPLEX_ITERS << " iters, total "
                     << us << " us, avg " << us / COMPLEX_ITERS << " us/iter";
}

// clang-format off
static const GroupByDateParam kGroupByDateParams[] = {
    // 18 cells: 3 mime × 3 granularity × 2 root counts  (excludeSens=false)
    {MIME_TYPE_VIDEO,             DateSectionGranularity::Day,   1, false},
    {MIME_TYPE_VIDEO,             DateSectionGranularity::Day,   3, false},
    {MIME_TYPE_VIDEO,             DateSectionGranularity::Month, 1, false},
    {MIME_TYPE_VIDEO,             DateSectionGranularity::Month, 3, false},
    {MIME_TYPE_VIDEO,             DateSectionGranularity::Year,  1, false},
    {MIME_TYPE_VIDEO,             DateSectionGranularity::Year,  3, false},
    {MIME_TYPE_ALL_VISUAL_MEDIA,  DateSectionGranularity::Day,   1, false},
    {MIME_TYPE_ALL_VISUAL_MEDIA,  DateSectionGranularity::Day,   3, false},
    {MIME_TYPE_ALL_VISUAL_MEDIA,  DateSectionGranularity::Month, 1, false},
    {MIME_TYPE_ALL_VISUAL_MEDIA,  DateSectionGranularity::Month, 3, false},
    {MIME_TYPE_ALL_VISUAL_MEDIA,  DateSectionGranularity::Year,  1, false},
    {MIME_TYPE_ALL_VISUAL_MEDIA,  DateSectionGranularity::Year,  3, false},
    {MIME_TYPE_ALL_DOCS,          DateSectionGranularity::Day,   1, false},
    {MIME_TYPE_ALL_DOCS,          DateSectionGranularity::Day,   3, false},
    {MIME_TYPE_ALL_DOCS,          DateSectionGranularity::Month, 1, false},
    {MIME_TYPE_ALL_DOCS,          DateSectionGranularity::Month, 3, false},
    {MIME_TYPE_ALL_DOCS,          DateSectionGranularity::Year,  1, false},
    {MIME_TYPE_ALL_DOCS,          DateSectionGranularity::Year,  3, false},
    // 4 spot checks with excludeSensitive=true — only on Day granularity
    // (sens-pruning cost depends on tree walk, not on bucket count).
    {MIME_TYPE_VIDEO,             DateSectionGranularity::Day,   1, true},
    {MIME_TYPE_ALL_VISUAL_MEDIA,  DateSectionGranularity::Day,   1, true},
    {MIME_TYPE_ALL_DOCS,          DateSectionGranularity::Day,   1, true},
    {MIME_TYPE_VIDEO,             DateSectionGranularity::Day,   3, true},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(All,
                         SqliteNodesPerfGroupByDateTest,
                         ::testing::ValuesIn(kGroupByDateParams));

// ═══════════════════════════════════════════════════════════════════════════
//  Phase 3: listAllNodesByPage + byTimestampAnchor parametric perf
// ═══════════════════════════════════════════════════════════════════════════
//
// Query-plan note: no post-populate ANALYZE (would need a production API
// change). SQLite defaults + createIndexes() match what production sees on a
// fresh DB.

enum class AnchorPosition
{
    None,
    First, // anchor at first bucket — DESC anchor returns ~all rows
    Mid, // anchor at mid bucket — DESC anchor returns ~half
    Last, // anchor at last bucket — DESC anchor returns ~few
};

struct ListAllAnchorParam
{
    MimeType_t mimeType;
    int order; // OrderByClause value
    AnchorPosition anchor;
    bool hasCursor;
};

class SqliteNodesPerfListAllByAnchorTest:
    public SqliteDateSectionsPerfFixture,
    public ::testing::WithParamInterface<ListAllAnchorParam>
{};

static const char* anchorName(AnchorPosition a)
{
    switch (a)
    {
        case AnchorPosition::None:
            return "None";
        case AnchorPosition::First:
            return "First";
        case AnchorPosition::Mid:
            return "Mid";
        case AnchorPosition::Last:
            return "Last";
    }
    return "?";
}

static const char* orderName(int o)
{
    switch (o)
    {
        case OrderByClause::MTIME_ASC:
            return "MTIME_ASC";
        case OrderByClause::MTIME_DESC:
            return "MTIME_DESC";
        case OrderByClause::SIZE_DESC:
            return "SIZE_DESC";
        default:
            return "OTHER";
    }
}

TEST_P(SqliteNodesPerfListAllByAnchorTest, DISABLED_Perf)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const auto& p = GetParam();
    constexpr size_t pageSize = 50;

    ListAllNodesParams lparams;
    lparams.mimeType = p.mimeType;
    lparams.order = p.order;
    lparams.maxElements = pageSize;
    lparams.excludeSensitive = false;

    // Anchor selection — picks a bucket-start mtime; the half-bound's
    // selectivity is determined by the bucket position + anchor direction.
    if (p.anchor != AnchorPosition::None)
    {
        TimestampAnchorFilter anchor;
        anchor.mOrder = p.order; // anchor direction follows page order
        switch (p.anchor)
        {
            case AnchorPosition::First:
                anchor.mStartSeconds = mFirstBucketStartSec;
                anchor.mEndSeconds = mFirstBucketStartSec + 86400;
                break;
            case AnchorPosition::Mid:
                anchor.mStartSeconds = mMidBucketStartSec;
                anchor.mEndSeconds = mMidBucketStartSec + 86400;
                break;
            case AnchorPosition::Last:
                anchor.mStartSeconds = mLastBucketStartSec;
                anchor.mEndSeconds = mLastBucketStartSec + 86400;
                break;
            case AnchorPosition::None:
                break;
        }
        lparams.timestampAnchor = anchor;
    }

    const std::vector<NodeHandle> roots{mRootHandle};

    // Build a cursor anchored mid-way for the hasCursor variants.
    if (p.hasCursor)
    {
        NodeSearchCursorOffset c;
        c.mLastHandle = mFileHandles[mFileHandles.size() / 2].as8byte();
        c.mLastName = "f_50_500.jpg"; // approximate; cursor just needs a row past which to walk
        if (p.order == OrderByClause::MTIME_ASC || p.order == OrderByClause::MTIME_DESC)
            c.mLastMtime = mMidBucketStartSec;
        lparams.cursor = c;
    }

    // Warm prepare.
    {
        std::vector<std::pair<NodeHandle, NodeSerialized>> warm;
        CancelToken ct;
        table->listAllNodesByPage(lparams, roots, warm, ct);
    }

    size_t lastResultSize = 0;
    const long long us = measureUs(COMPLEX_ITERS,
                                   [&]
                                   {
                                       std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
                                       CancelToken ct;
                                       table->listAllNodesByPage(lparams, roots, nodes, ct);
                                       lastResultSize = nodes.size();
                                   });

    GTEST_LOG_(INFO) << "listAllByPage [mime=" << mimeName(p.mimeType)
                     << ", order=" << orderName(p.order) << ", anchor=" << anchorName(p.anchor)
                     << ", cursor=" << (p.hasCursor ? "1" : "0") << ", page=" << pageSize
                     << ", returned=" << lastResultSize << "]: " << COMPLEX_ITERS
                     << " iters, total " << us << " us, avg " << us / COMPLEX_ITERS << " us/iter";
}

// clang-format off
static const ListAllAnchorParam kListAllAnchorParams[] = {
    // 16 cells without cursor: 2 mime × 2 order × 4 anchor positions
    {MIME_TYPE_VIDEO,             OrderByClause::MTIME_DESC, AnchorPosition::None,  false},
    {MIME_TYPE_VIDEO,             OrderByClause::MTIME_DESC, AnchorPosition::First, false},
    {MIME_TYPE_VIDEO,             OrderByClause::MTIME_DESC, AnchorPosition::Mid,   false},
    {MIME_TYPE_VIDEO,             OrderByClause::MTIME_DESC, AnchorPosition::Last,  false},
    {MIME_TYPE_VIDEO,             OrderByClause::MTIME_ASC,  AnchorPosition::None,  false},
    {MIME_TYPE_VIDEO,             OrderByClause::MTIME_ASC,  AnchorPosition::First, false},
    {MIME_TYPE_VIDEO,             OrderByClause::MTIME_ASC,  AnchorPosition::Mid,   false},
    {MIME_TYPE_VIDEO,             OrderByClause::MTIME_ASC,  AnchorPosition::Last,  false},
    {MIME_TYPE_ALL_VISUAL_MEDIA,  OrderByClause::MTIME_DESC, AnchorPosition::None,  false},
    {MIME_TYPE_ALL_VISUAL_MEDIA,  OrderByClause::MTIME_DESC, AnchorPosition::First, false},
    {MIME_TYPE_ALL_VISUAL_MEDIA,  OrderByClause::MTIME_DESC, AnchorPosition::Mid,   false},
    {MIME_TYPE_ALL_VISUAL_MEDIA,  OrderByClause::MTIME_DESC, AnchorPosition::Last,  false},
    {MIME_TYPE_ALL_VISUAL_MEDIA,  OrderByClause::MTIME_ASC,  AnchorPosition::None,  false},
    {MIME_TYPE_ALL_VISUAL_MEDIA,  OrderByClause::MTIME_ASC,  AnchorPosition::First, false},
    {MIME_TYPE_ALL_VISUAL_MEDIA,  OrderByClause::MTIME_ASC,  AnchorPosition::Mid,   false},
    {MIME_TYPE_ALL_VISUAL_MEDIA,  OrderByClause::MTIME_ASC,  AnchorPosition::Last,  false},
    // 4 cursor spot checks at Anchor=Mid (hardest planner case)
    {MIME_TYPE_VIDEO,             OrderByClause::MTIME_DESC, AnchorPosition::Mid,   true},
    {MIME_TYPE_VIDEO,             OrderByClause::MTIME_ASC,  AnchorPosition::Mid,   true},
    {MIME_TYPE_ALL_VISUAL_MEDIA,  OrderByClause::MTIME_DESC, AnchorPosition::Mid,   true},
    {MIME_TYPE_ALL_VISUAL_MEDIA,  OrderByClause::MTIME_ASC,  AnchorPosition::Mid,   true},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(All,
                         SqliteNodesPerfListAllByAnchorTest,
                         ::testing::ValuesIn(kListAllAnchorParams));

// ═══════════════════════════════════════════════════════════════════════════
//  Phase 4: Prepared-statement cache effects (smaller fixture)
// ═══════════════════════════════════════════════════════════════════════════
//
// Uses kSmallParams (5000 nodes / 30 days) so the per-iter rebuild of
// SqliteAccountState stays cheap (<0.5s vs ~30s at 100K), allowing ~30-50 iters.
//
// "Cold prepare" measures the first query against a freshly-rebuilt state: a
// cache-miss sqlite3_prepare_v2 plus the first exec. The client rebuild + DB open
// happen in SetUp(), OUTSIDE the timed window — only the first prepare+exec is timed.

class SqliteCacheEffectsPerfFixture: public DateSpreadFixtureBase
{
protected:
    const DateSpreadParams& params() const override
    {
        return kSmallParams;
    }

    const char* fixtureName() const override
    {
        return "sqlite_cache_effects_perf";
    }
};

using DISABLED_SqliteCacheEffectsPerfTest = SqliteCacheEffectsPerfFixture;

TEST_F(DISABLED_SqliteCacheEffectsPerfTest, GroupByDate_WarmReuse)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    DateSectionParams sp;
    sp.mimeType = MIME_TYPE_ALL_VISUAL_MEDIA;
    sp.order = OrderByClause::MTIME_DESC;
    sp.granularity = DateSectionGranularity::Day;

    const std::vector<NodeHandle> roots{mRootHandle};

    {
        std::vector<DateSection> warm;
        CancelToken ct;
        table->groupAllNodesByDate(sp, roots, warm, ct);
    }

    const long long us = measureUs(SIMPLE_ITERS,
                                   [&]
                                   {
                                       std::vector<DateSection> sections;
                                       CancelToken ct;
                                       table->groupAllNodesByDate(sp, roots, sections, ct);
                                   });

    GTEST_LOG_(INFO) << "GroupByDate_WarmReuse [" << kSmallNumFiles << " nodes / " << kSmallNumDays
                     << " days, Day granularity]: " << SIMPLE_ITERS << " iters, total " << us
                     << " us, avg " << us / SIMPLE_ITERS << " us/iter";
}

TEST_F(DISABLED_SqliteCacheEffectsPerfTest, GroupByDate_ColdPrepareViaRebuild)
{
    // Cold prepare = full first-call cost (client rebuild + DB open + first prepare).
    constexpr int kColdIters = 30;
    long long totalUs = 0;

    for (int iter = 0; iter < kColdIters; ++iter)
    {
        // Reset everything from the base fixture and re-populate.
        TearDown();
        mNextHandle = 1;
        mTopFolderHandles.clear();
        mSubFolderHandles.clear();
        mFileHandles.clear();
        SetUp();

        auto* table = nodesTable();
        ASSERT_NE(table, nullptr);

        DateSectionParams sp;
        sp.mimeType = MIME_TYPE_ALL_VISUAL_MEDIA;
        sp.order = OrderByClause::MTIME_DESC;
        sp.granularity = DateSectionGranularity::Day;
        const std::vector<NodeHandle> roots{mRootHandle};

        std::vector<DateSection> sections;
        CancelToken ct;
        auto t0 = std::chrono::steady_clock::now();
        table->groupAllNodesByDate(sp, roots, sections, ct);
        auto t1 = std::chrono::steady_clock::now();
        totalUs += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    }

    GTEST_LOG_(INFO) << "GroupByDate_ColdPrepareViaRebuild [" << kSmallNumFiles << " nodes / "
                     << kSmallNumDays << " days, Day granularity]: " << kColdIters
                     << " full-rebuild iters, avg cold first-call " << totalUs / kColdIters
                     << " us/iter (includes SqliteAccountState ctor + DB open + prepare)";
}

TEST_F(DISABLED_SqliteCacheEffectsPerfTest, GroupByDate_ShapeSwapBothCached)
{
    // Alternates between two distinct cache keys (different granularities). Both
    // shapes are pre-warmed below, so the timed loop measures the cost of
    // alternating between two CACHED statements (cache hits + lookup), NOT a miss.
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    DateSectionParams day;
    day.mimeType = MIME_TYPE_ALL_VISUAL_MEDIA;
    day.order = OrderByClause::MTIME_DESC;
    day.granularity = DateSectionGranularity::Day;

    DateSectionParams month = day;
    month.granularity = DateSectionGranularity::Month;

    const std::vector<NodeHandle> roots{mRootHandle};

    // Warm prepare; the timed loop measures execution only.
    {
        std::vector<DateSection> s;
        CancelToken ct;
        table->groupAllNodesByDate(day, roots, s, ct);
        table->groupAllNodesByDate(month, roots, s, ct);
    }

    const long long us = measureUs(SIMPLE_ITERS,
                                   [&]
                                   {
                                       std::vector<DateSection> sections;
                                       CancelToken ct;
                                       table->groupAllNodesByDate(day, roots, sections, ct);
                                       table->groupAllNodesByDate(month, roots, sections, ct);
                                   });

    GTEST_LOG_(INFO) << "GroupByDate_ShapeSwapBothCached [Day↔Month alternating, both pre-warmed]: "
                     << SIMPLE_ITERS << " swap pairs, total " << us << " us, avg "
                     << us / (SIMPLE_ITERS * 2) << " us per single-shape call";
}

TEST_F(DISABLED_SqliteCacheEffectsPerfTest, GroupByDate_MapGrowthObservation)
{
    // Observational: logs distinct-shape count; no assertion.
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const std::vector<NodeHandle> roots1{mRootHandle};
    std::vector<NodeHandle> roots2{mRootHandle};
    if (!mTopFolderHandles.empty())
        roots2.push_back(mTopFolderHandles[0]);

    int shapesIssued = 0;
    for (auto mime: {MIME_TYPE_VIDEO, MIME_TYPE_ALL_VISUAL_MEDIA, MIME_TYPE_ALL_DOCS})
        for (auto gran: {DateSectionGranularity::Day,
                         DateSectionGranularity::Month,
                         DateSectionGranularity::Year})
            for (bool sens: {false, true})
            {
                DateSectionParams sp;
                sp.mimeType = mime;
                sp.order = OrderByClause::MTIME_DESC;
                sp.granularity = gran;
                sp.excludeSensitive = sens;

                std::vector<DateSection> sections;
                CancelToken ct;
                table->groupAllNodesByDate(sp, roots1, sections, ct);
                table->groupAllNodesByDate(sp, roots2, sections, ct);
                shapesIssued += 2;
            }

    GTEST_LOG_(INFO) << "GroupByDate_MapGrowthObservation: issued " << shapesIssued
                     << " distinct shape calls (mime × gran × sens × roots). "
                     << "mStmtDateSections map size is private; reviewer should sample with a "
                     << "debugger if exact value matters. Each cached entry holds one "
                     << "sqlite3_stmt* (~few KB SQLite-internal). At ~50 entries the "
                     << "total memory is small but unbounded growth in a long-running client "
                     << "could matter — track separately if customer-facing concern surfaces.";
}

// ═══════════════════════════════════════════════════════════════════════════
//  Phase 5: offset-depth cost (real query latency vs offset)
// ═══════════════════════════════════════════════════════════════════════════
//
// Arm A: global offset depth curve (no anchor).
//   Sweeps offset values {0, 100, 1000, 10000, 90000} with a fixed page of
//   60 photos ordered MTIME_DESC against the full 100K-node dataset.
//   Expected: latency grows linearly with offset (O(offset) SQLite OFFSET).
//
// Arm B: anchored deep jump.
//   Fixes offset=100 and varies the timestamp anchor across early/mid/late day
//   buckets, using bounds from DateSpreadFixtureBase::populateDB:
//     mFirstBucketStartSec / mMidBucketStartSec / mLastBucketStartSec = midnight
//     UTC of day 0 / numDays/2 (~365) / numDays-1 (~730); each window is one day.
//   Expected: the anchor collapses an otherwise-deep seek (oldest bucket) to
//   offset=100 within one day — O(1 bucket), independent of bucket depth.
//
// Arm C: keyset (cursor) page cost at the same depths as Arm A.
//   Positions a cursor at each depth and times one page; expected to stay flat
//   (index seek), the direct contrast to Arm A's O(offset) growth.

class SqliteDateSectionsPerfPhase5Fixture: public SqliteDateSectionsPerfFixture
{};

using DISABLED_Offset_GlobalDepthCurve = SqliteDateSectionsPerfPhase5Fixture;
using DISABLED_Offset_AnchoredDeepJump = SqliteDateSectionsPerfPhase5Fixture;
using DISABLED_Offset_CursorDepthCurve = SqliteDateSectionsPerfPhase5Fixture;

TEST_F(DISABLED_Offset_GlobalDepthCurve, GlobalOffsetDepthCurve)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const std::vector<NodeHandle> roots{mRootHandle};
    constexpr size_t pageSize = 60;

    // Each offset is measured once (not averaged): this is a single-shot
    // cost probe, not a steady-state benchmark.  The DB call itself is what
    // is timed — no volatile sink, no dead-code elimination concern.
    for (const int64_t offset: {0LL, 100LL, 1000LL, 10000LL, 90000LL})
    {
        ListAllNodesParams lparams;
        lparams.mimeType = MIME_TYPE_PHOTO;
        lparams.order = OrderByClause::MTIME_DESC;
        lparams.maxElements = pageSize;
        lparams.excludeSensitive = false;
        lparams.offset = offset;

        // Warm prepare on offset=0 so statement caching doesn't inflate the
        // first probe (only relevant for the very first iteration).
        if (offset == 0)
        {
            std::vector<std::pair<NodeHandle, NodeSerialized>> warm;
            CancelToken ct;
            table->listAllNodesByPage(lparams, roots, warm, ct);
        }

        std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
        CancelToken ct;
        const auto t0 = std::chrono::steady_clock::now();
        table->listAllNodesByPage(lparams, roots, nodes, ct);
        const auto t1 = std::chrono::steady_clock::now();
        const long long us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

        GTEST_LOG_(INFO) << "GlobalOffsetDepthCurve [offset=" << offset
                         << ", returned=" << nodes.size() << "]: " << us << " us";
    }
}

TEST_F(DISABLED_Offset_AnchoredDeepJump, AnchoredDeepJump)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const std::vector<NodeHandle> roots{mRootHandle};
    constexpr size_t pageSize = 60;
    constexpr int64_t kFixedOffset = 100;
    constexpr int64_t kOneDaySecs = 86400LL;

    // Anchor positions: early (day 0), mid (~day 365), late (day 730).
    // Bucket bounds are snapped-to-midnight values computed in populateDB.
    struct AnchorCase
    {
        const char* label;
        int64_t startSec;
    };

    const AnchorCase cases[] = {
        {"early(day0)", mFirstBucketStartSec},
        {"mid(day~365)", mMidBucketStartSec},
        {"late(day730)", mLastBucketStartSec},
    };

    // Warm prepare (no anchor, no offset) so the prepared-statement cache is
    // populated before we enter the timed section.
    {
        ListAllNodesParams warm;
        warm.mimeType = MIME_TYPE_PHOTO;
        warm.order = OrderByClause::MTIME_DESC;
        warm.maxElements = pageSize;
        std::vector<std::pair<NodeHandle, NodeSerialized>> warmOut;
        CancelToken ct;
        table->listAllNodesByPage(warm, roots, warmOut, ct);
    }

    for (const auto& c: cases)
    {
        ListAllNodesParams lparams;
        lparams.mimeType = MIME_TYPE_PHOTO;
        lparams.order = OrderByClause::MTIME_DESC;
        lparams.maxElements = pageSize;
        lparams.excludeSensitive = false;
        lparams.offset = kFixedOffset;

        TimestampAnchorFilter anchor;
        anchor.mOrder = OrderByClause::MTIME_DESC;
        anchor.mStartSeconds = c.startSec;
        anchor.mEndSeconds = c.startSec + kOneDaySecs;
        lparams.timestampAnchor = anchor;

        std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
        CancelToken ct;
        const auto t0 = std::chrono::steady_clock::now();
        table->listAllNodesByPage(lparams, roots, nodes, ct);
        const auto t1 = std::chrono::steady_clock::now();
        const long long us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

        GTEST_LOG_(INFO) << "AnchoredDeepJump [anchor=" << c.label << ", offset=" << kFixedOffset
                         << ", returned=" << nodes.size() << "]: " << us << " us";
    }
}

// Arm C: keyset (cursor) page cost at the same depths as Arm A. A cursor seeks via
// index to its position and reads pageSize rows, so the page cost should stay flat
// regardless of depth — the contrast against Arm A's O(offset) growth.
TEST_F(DISABLED_Offset_CursorDepthCurve, CursorDepthCurve)
{
    auto* table = nodesTable();
    ASSERT_NE(table, nullptr);

    const std::vector<NodeHandle> roots{mRootHandle};
    constexpr size_t pageSize = 60;

    auto fetch = [&](const ListAllNodesParams& p)
    {
        std::vector<std::pair<NodeHandle, NodeSerialized>> out;
        CancelToken ct;
        table->listAllNodesByPage(p, roots, out, ct);
        return out;
    };

    // Warm prepare (first page, no cursor).
    {
        ListAllNodesParams warm;
        warm.mimeType = MIME_TYPE_PHOTO;
        warm.order = OrderByClause::MTIME_DESC;
        warm.maxElements = pageSize;
        fetch(warm);
    }

    for (const int64_t depth: {0LL, 100LL, 1000LL, 10000LL})
    {
        ListAllNodesParams lparams;
        lparams.mimeType = MIME_TYPE_PHOTO;
        lparams.order = OrderByClause::MTIME_DESC;
        lparams.maxElements = pageSize;

        // Untimed setup: position a keyset cursor just before `depth` by finding the
        // node at rank depth-1 (offset probe) and building a cursor from it. Only the
        // cursor fetch below is timed.
        if (depth > 0)
        {
            ListAllNodesParams probe;
            probe.mimeType = MIME_TYPE_PHOTO;
            probe.order = OrderByClause::MTIME_DESC;
            probe.maxElements = 1;
            probe.offset = depth - 1;
            const auto boundary = fetch(probe);
            ASSERT_EQ(boundary.size(), 1u) << "depth=" << depth;

            const auto n = mClient->mNodeManager.getNodeByHandle(boundary.front().first);
            ASSERT_NE(n, nullptr);

            NodeSearchCursorOffset c;
            c.mLastName = n->displayname();
            c.mLastHandle = n->nodeHandle().as8byte();
            c.mLastMtime = n->mtime;
            lparams.cursor = c;
        }

        std::vector<std::pair<NodeHandle, NodeSerialized>> nodes;
        CancelToken ct;
        const auto t0 = std::chrono::steady_clock::now();
        table->listAllNodesByPage(lparams, roots, nodes, ct);
        const auto t1 = std::chrono::steady_clock::now();
        const long long us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

        GTEST_LOG_(INFO) << "CursorDepthCurve [depth=" << depth << ", returned=" << nodes.size()
                         << "]: " << us << " us";
    }
}

} // anonymous namespace

#endif // USE_SQLITE
