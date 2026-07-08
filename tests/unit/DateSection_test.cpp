/**
 * @file DateSection_test.cpp
 * @brief Unit tests for SDK-6171 `groupAllNodesByDate` + `byTimestampAnchor`.
 *
 * Split from Sqlite_test.cpp; exercises groupAllNodesByDate + byTimestampAnchor
 * against real SQLite. Tests inherit from SearchByPageTest (SearchByPageTestBase.h)
 * and override populateDB() to add a date-bucketed subtree on top of the base
 * dataset.
 */

#include "utils.h"

#include <gtest/gtest.h>
#include <mega/db/sqlite.h>
#include <mega/megaapp.h>
#include <mega/megaclient.h>
#include <mega/nodemanager.h>

#include <mega.h> // brings in config.h which #defines USE_SQLITE when enabled
#include <optional>
#include <string>
#include <vector>

using namespace mega;

#ifdef USE_SQLITE

#include "SearchByPageTestBase.h"

namespace fs = std::filesystem;

namespace
{

using namespace mega::pagetest;

// ═══════════════════════════════════════════════════════════════════════════
//  DateSectionTest – fixture + tests for groupAllNodesByDate +
//  byTimestampAnchor SQL semantics. Real SQLite via SearchByPageTest.
// ═══════════════════════════════════════════════════════════════════════════
//
// Named DateSectionTest to avoid shadowing production mega::DateSection (using
// namespace mega).
//
// Pinned UTC mtimes (seconds since epoch):
//   1704067200  = 2024-01-01 00:00:00 UTC   (jan01.jpg)
//   1709164800  = 2024-02-29 00:00:00 UTC   (feb29.jpg)        // leap day
//   1717200000  = 2024-06-01 00:00:00 UTC   (jun01.jpg)
//   1719792000  = 2024-07-01 00:00:00 UTC   (jul01.jpg)        // July bucket start
//   1720224000  = 2024-07-06 00:00:00 UTC   (julSensitive.jpg) // sensitive
//   1721001600  = 2024-07-15 12:00:00 UTC   (jul15.jpg)
//   1722470399  = 2024-07-31 23:59:59 UTC   (jul31.jpg)        // bucket end - 1s
//   1722470400  = 2024-08-01 00:00:00 UTC   (aug01.jpg)        // next bucket
//   1735603200  = 2024-12-31 00:00:00 UTC   (dec31_2024.jpg)   // year rollover
//   1703980800  = 2023-12-31 00:00:00 UTC   (dec31.jpg)
//   0           = mega_invalid_timestamp    (epoch.jpg)        // must be excluded
//   -1                                       (negMtime.jpg)    // must be excluded
//
// Plus videos for grouped-mime tests:
//   1721001600  = 2024-07-15 12:00:00 UTC   (jul_vid1.mp4)
//   1721174400  = 2024-07-17 12:00:00 UTC   (jul_vid2.mp4)
//
// All photo nodes live under mDateSectionRoot so the base fixture's JPGs
// don't contaminate counts; queries scope to {mDateSectionRoot}.

class DateSectionTest: public SearchByPageTest
{
protected:
    NodeHandle mDateSectionRoot;
    NodeHandle mSubFolderA; ///< subfolder for excludeHandles tests
    NodeHandle mJulSensitive;
    NodeHandle mJulInSubA; ///< July photo placed under mSubFolderA

    void populateDB() override
    {
        SearchByPageTest::populateDB();

        auto root = mClient->mNodeManager.getNodeByHandle(mRootHandle);
        ASSERT_NE(root, nullptr);

        auto subtree = addNode(FOLDERNODE, root, NodeMeta{"date_section_subtree", FOLDERNODE});
        mDateSectionRoot = subtree->nodeHandle();

        auto subA = addNode(FOLDERNODE, subtree, NodeMeta{"subA", FOLDERNODE});
        mSubFolderA = subA->nodeHandle();

        // Distinct sizes per photo so the ORDER_SIZE_DESC orthogonality test
        // produces a stable order.
        addNode(FILENODE, subtree, NodeMeta{"jan01.jpg", FILENODE, 100, 1704067200});
        addNode(FILENODE, subtree, NodeMeta{"feb29.jpg", FILENODE, 110, 1709164800});
        addNode(FILENODE, subtree, NodeMeta{"jun01.jpg", FILENODE, 200, 1717200000});
        addNode(FILENODE, subtree, NodeMeta{"jul01.jpg", FILENODE, 300, 1719792000});
        addNode(FILENODE, subtree, NodeMeta{"jul15.jpg", FILENODE, 400, 1721001600});
        addNode(FILENODE, subtree, NodeMeta{"jul31.jpg", FILENODE, 500, 1722470399});
        addNode(FILENODE, subtree, NodeMeta{"aug01.jpg", FILENODE, 600, 1722470400});
        addNode(FILENODE, subtree, NodeMeta{"dec31.jpg", FILENODE, 700, 1703980800});
        addNode(FILENODE, subtree, NodeMeta{"dec31_2024.jpg", FILENODE, 900, 1735603200});

        // Epoch / negative mtime — must be excluded from all section queries.
        addNode(FILENODE, subtree, NodeMeta{"epoch.jpg", FILENODE, 800, 0});
        addNode(FILENODE, subtree, NodeMeta{"negMtime.jpg", FILENODE, 820, -1});

        // Sensitive photo in July (sized between jul15 and jul31).
        NodeMeta sens{"julSensitive.jpg", FILENODE, 450, 1720224000};
        sens.sensitive = true;
        mJulSensitive = addNode(FILENODE, subtree, sens)->nodeHandle();

        // Photo under subA so excludeHandles=[subA] / explicitAncestors=[subA]
        // tests can target it.
        mJulInSubA = addNode(FILENODE, subA, NodeMeta{"jul_subA.jpg", FILENODE, 460, 1720310400})
                         ->nodeHandle();

        // Videos for grouped-mime tests (.mp4).
        addNode(FILENODE, subtree, NodeMeta{"jul_vid1.mp4", FILENODE, 1000, 1721001600});
        addNode(FILENODE, subtree, NodeMeta{"jul_vid2.mp4", FILENODE, 1100, 1721174400});
    }

    DBTableNodes* tableNodes()
    {
        return dynamic_cast<DBTableNodes*>(mClient->sctable.get());
    }

    // Invoke DBTableNodes::groupAllNodesByDate directly with the date-section
    // subtree as the single root.
    std::vector<DateSection> runSections(DateSectionGranularity g,
                                         int order = OrderByClause::MTIME_DESC,
                                         MimeType_t mime = MIME_TYPE_PHOTO,
                                         bool excludeSensitive = false,
                                         std::vector<NodeHandle> excludeHandles = {},
                                         std::vector<NodeHandle> roots = {})
    {
        DateSectionParams p;
        p.mimeType = mime;
        p.order = order;
        p.granularity = g;
        p.excludeSensitive = excludeSensitive;
        p.excludeHandles = std::move(excludeHandles);

        std::vector<DateSection> out;
        const std::vector<NodeHandle> filesRoots =
            roots.empty() ? std::vector<NodeHandle>{mDateSectionRoot} : std::move(roots);
        tableNodes()->groupAllNodesByDate(p, filesRoots, out, CancelToken{});
        return out;
    }

    std::vector<std::pair<NodeHandle, NodeSerialized>>
        runListAll(int order,
                   std::optional<TimestampAnchorFilter> anchor = std::nullopt,
                   size_t maxElements = 0,
                   MimeType_t mime = MIME_TYPE_PHOTO,
                   bool excludeSensitive = false)
    {
        auto p = makeParams(mime,
                            order,
                            maxElements,
                            excludeSensitive,
                            /*cursor=*/std::nullopt,
                            /*explicitAncestors=*/{mDateSectionRoot},
                            /*excludeHandles=*/{},
                            /*locationScope=*/1);
        p.timestampAnchor = anchor;

        std::vector<std::pair<NodeHandle, NodeSerialized>> out;
        tableNodes()->listAllNodesByPage(p, {mDateSectionRoot}, out, CancelToken{});
        return out;
    }

    // (gid, count) tuples for compact section-list assertions.
    static std::vector<std::pair<std::string, int64_t>>
        gidCounts(const std::vector<DateSection>& sections)
    {
        std::vector<std::pair<std::string, int64_t>> result;
        result.reserve(sections.size());
        for (const auto& s: sections)
            result.emplace_back(s.mGroupId, s.mCount);
        return result;
    }

    static const DateSection* find(const std::vector<DateSection>& v, const std::string& gid)
    {
        for (const auto& s: v)
            if (s.mGroupId == gid)
                return &s;
        return nullptr;
    }

    // NodeSerialized.mNode is a serialized blob; for mtime/size assertions
    // we look the node up live via NodeManager.
    bool hasMtime(const std::vector<std::pair<NodeHandle, NodeSerialized>>& rows, int64_t mtime)
    {
        for (const auto& [h, _]: rows)
        {
            auto n = mClient->mNodeManager.getNodeByHandle(h);
            if (n && n->mtime == mtime)
                return true;
        }
        return false;
    }

    // mtimes in row order, looked up via NodeManager.
    std::vector<int64_t> mtimes(const std::vector<std::pair<NodeHandle, NodeSerialized>>& rows)
    {
        std::vector<int64_t> result;
        result.reserve(rows.size());
        for (const auto& [h, _]: rows)
        {
            auto n = mClient->mNodeManager.getNodeByHandle(h);
            result.push_back(n ? n->mtime : -1);
        }
        return result;
    }

    // sizes in row order, looked up via NodeManager.
    std::vector<int64_t> sizes(const std::vector<std::pair<NodeHandle, NodeSerialized>>& rows)
    {
        std::vector<int64_t> result;
        result.reserve(rows.size());
        for (const auto& [h, _]: rows)
        {
            auto n = mClient->mNodeManager.getNodeByHandle(h);
            result.push_back(n ? n->size : -1);
        }
        return result;
    }
};

// ─── B1: section shape (gids, counts, order) ────────────────────────────────

TEST_F(DateSectionTest, DateSection_MonthGranularity_PhotoFilter)
{
    // 11 photos (epoch + negMtime excluded; sensitive included by default).
    // July bucket holds 5: jul01, jul15, jul31, julSensitive, jul_subA.
    const auto v = runSections(DateSectionGranularity::Month);
    const auto tuples = gidCounts(v);

    EXPECT_EQ(tuples,
              (std::vector<std::pair<std::string, int64_t>>{{"2024-12", 1},
                                                            {"2024-08", 1},
                                                            {"2024-07", 5},
                                                            {"2024-06", 1},
                                                            {"2024-02", 1},
                                                            {"2024-01", 1},
                                                            {"2023-12", 1}}));
}

TEST_F(DateSectionTest, DateSection_YearGranularity_Aggregation)
{
    const auto v = runSections(DateSectionGranularity::Year);
    const auto tuples = gidCounts(v);

    // 2024 has 10 photos: jan01, feb29, jun01, jul01, jul15, jul31, aug01,
    // dec31_2024, julSensitive, jul_subA. 2023 has 1: dec31.
    EXPECT_EQ(tuples, (std::vector<std::pair<std::string, int64_t>>{{"2024", 10}, {"2023", 1}}));
}

TEST_F(DateSectionTest, DateSection_DayGranularity_PerDayBuckets)
{
    const auto v = runSections(DateSectionGranularity::Day);
    ASSERT_FALSE(v.empty());

    // Three distinct July days plus 1 in late July: jul01, jul15, jul31, jul06 (sens), jul07 (subA)
    auto* jul01 = find(v, "2024-07-01");
    ASSERT_NE(jul01, nullptr);
    EXPECT_EQ(jul01->mCount, 1);

    auto* jul15 = find(v, "2024-07-15");
    ASSERT_NE(jul15, nullptr);
    EXPECT_EQ(jul15->mCount, 1);

    auto* jul31 = find(v, "2024-07-31");
    ASSERT_NE(jul31, nullptr);
    EXPECT_EQ(jul31->mCount, 1);

    // jul06 (julSensitive) and jul07 (jul_subA) must land in their own day buckets,
    // not collapse into jul01: the julyTotal==5 sum below would survive that bug.
    auto* jul06 = find(v, "2024-07-06");
    ASSERT_NE(jul06, nullptr);
    EXPECT_EQ(jul06->mCount, 1);

    auto* jul07 = find(v, "2024-07-07");
    ASSERT_NE(jul07, nullptr);
    EXPECT_EQ(jul07->mCount, 1);

    // Total count must equal MONTH "2024-07" count (5).
    int64_t julyTotal = 0;
    for (const auto& s: v)
        if (s.mGroupId.find("2024-07-") == 0)
            julyTotal += s.mCount;
    EXPECT_EQ(julyTotal, 5);
}

TEST_F(DateSectionTest, DateSection_OrderAsc_OldestFirst)
{
    const auto v = runSections(DateSectionGranularity::Month, OrderByClause::MTIME_ASC);
    const auto tuples = gidCounts(v);

    EXPECT_EQ(tuples,
              (std::vector<std::pair<std::string, int64_t>>{{"2023-12", 1},
                                                            {"2024-01", 1},
                                                            {"2024-02", 1},
                                                            {"2024-06", 1},
                                                            {"2024-07", 5},
                                                            {"2024-08", 1},
                                                            {"2024-12", 1}}));
}

TEST_F(DateSectionTest, DateSection_GidStrftimeFormat_ZeroPadded)
{
    // January / February / single-digit days must be zero-padded.
    const auto month = runSections(DateSectionGranularity::Month);
    EXPECT_NE(find(month, "2024-01"), nullptr);
    EXPECT_NE(find(month, "2024-02"), nullptr);
    EXPECT_EQ(find(month, "2024-1"), nullptr);
    EXPECT_EQ(find(month, "2024-2"), nullptr);

    const auto day = runSections(DateSectionGranularity::Day);
    EXPECT_NE(find(day, "2024-07-01"), nullptr);
    EXPECT_EQ(find(day, "2024-7-1"), nullptr);
}

// ─── B2: bucket bounds correctness (the central new SQL responsibility) ────

TEST_F(DateSectionTest, DateSection_DayBounds_PinnedValues)
{
    const auto v = runSections(DateSectionGranularity::Day);
    auto* s = find(v, "2024-07-15");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->mStartDate, 1721001600); // 2024-07-15 00:00:00 UTC
    EXPECT_EQ(s->mEndDate, 1721088000); // 2024-07-16 00:00:00 UTC
}

TEST_F(DateSectionTest, DateSection_MonthBounds_PinnedValues)
{
    const auto v = runSections(DateSectionGranularity::Month);
    auto* s = find(v, "2024-07");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->mStartDate, 1719792000); // 2024-07-01 00:00:00 UTC
    EXPECT_EQ(s->mEndDate, 1722470400); // 2024-08-01 00:00:00 UTC
}

TEST_F(DateSectionTest, DateSection_YearBounds_PinnedValues)
{
    const auto v = runSections(DateSectionGranularity::Year);
    auto* s = find(v, "2024");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->mStartDate, 1704067200); // 2024-01-01 00:00:00 UTC
    EXPECT_EQ(s->mEndDate, 1735689600); // 2025-01-01 00:00:00 UTC
}

TEST_F(DateSectionTest, DateSection_MonthRollover_BucketEndIsNextYear)
{
    const auto v = runSections(DateSectionGranularity::Month);
    auto* s = find(v, "2024-12");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->mEndDate, 1735689600); // 2025-01-01 00:00:00 UTC
}

TEST_F(DateSectionTest, DateSection_DayRollover_BucketEndIsNextMonth)
{
    const auto v = runSections(DateSectionGranularity::Day);
    auto* s = find(v, "2024-12-31");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->mEndDate, 1735689600); // 2025-01-01 00:00:00 UTC
}

TEST_F(DateSectionTest, DateSection_LeapDay_BucketsAreAdjacent)
{
    const auto v = runSections(DateSectionGranularity::Day);

    auto* feb29 = find(v, "2024-02-29");
    ASSERT_NE(feb29, nullptr);
    EXPECT_EQ(feb29->mStartDate, 1709164800); // 2024-02-29 00:00:00 UTC
    EXPECT_EQ(feb29->mEndDate, 1709251200); // 2024-03-01 00:00:00 UTC
}

TEST_F(DateSectionTest, DateSection_BoundsAreInt64NotText)
{
    // Mirror the outer SELECT's CAST(... AS INTEGER) and confirm the column
    // type is SQLITE_INTEGER. Without the CAST it would be TEXT and
    // sqlite3_column_int64 would silently parse, hiding regressions.
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT CAST(strftime('%s', '2024-07' || '-01 00:00:00') AS INTEGER)";
    ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_type(stmt, 0), SQLITE_INTEGER);
    EXPECT_EQ(sqlite3_column_int64(stmt, 0), 1719792000);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

// ─── B3: mtime <= 0 exclusion ──────────────────────────────────────────────

TEST_F(DateSectionTest, DateSection_MtimeZero_NoEpochBucket)
{
    const auto v = runSections(DateSectionGranularity::Month);
    for (const auto& s: v)
    {
        EXPECT_NE(s.mGroupId.substr(0, 4), "1970")
            << "epoch.jpg leaked into the section list: " << s.mGroupId;
    }
}

TEST_F(DateSectionTest, DateSection_MtimeNegative_Excluded)
{
    // epoch.jpg (mtime=0) and negMtime.jpg (mtime=-1) must be excluded by the
    // `> invalidSentinel` guard, leaving 11. The year-range pins catch a buggy
    // guard letting mtime=-1 through, which would yield a far-future or
    // negative-year gid from strftime.
    const auto v = runSections(DateSectionGranularity::Year);
    for (const auto& s: v)
    {
        EXPECT_GE(s.mGroupId, "1970");
        EXPECT_LT(s.mGroupId, "2100");
    }
    int64_t total = 0;
    for (const auto& s: v)
        total += s.mCount;
    EXPECT_EQ(total, 11);
}

// ─── B4: WHERE-clause filters ──────────────────────────────────────────────

TEST_F(DateSectionTest, DateSection_ExcludeSensitive_HidesNodes)
{
    const auto v = runSections(DateSectionGranularity::Month,
                               OrderByClause::MTIME_DESC,
                               MIME_TYPE_PHOTO,
                               /*excludeSensitive=*/true);

    auto* jul = find(v, "2024-07");
    ASSERT_NE(jul, nullptr);
    EXPECT_EQ(jul->mCount, 4); // 5 normally, minus julSensitive
}

TEST_F(DateSectionTest, DateSection_ExcludeHandles_DropsSubtree)
{
    // Excluding subA must remove its July photo (jul_subA.jpg) from "2024-07".
    const auto v = runSections(DateSectionGranularity::Month,
                               OrderByClause::MTIME_DESC,
                               MIME_TYPE_PHOTO,
                               /*excludeSensitive=*/false,
                               /*excludeHandles=*/{mSubFolderA});

    auto* jul = find(v, "2024-07");
    ASSERT_NE(jul, nullptr);
    EXPECT_EQ(jul->mCount, 4); // 5 normally, minus jul_subA
}

TEST_F(DateSectionTest, DateSection_ByLocationHandles_NarrowToSubfolder)
{
    // Scope to subA only — should contain just jul_subA.jpg.
    const auto v = runSections(DateSectionGranularity::Month,
                               OrderByClause::MTIME_DESC,
                               MIME_TYPE_PHOTO,
                               /*excludeSensitive=*/false,
                               /*excludeHandles=*/{},
                               /*roots=*/{mSubFolderA});

    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v.front().mGroupId, "2024-07");
    EXPECT_EQ(v.front().mCount, 1);
}

TEST_F(DateSectionTest, DateSection_EmptyFilesRoots_ReturnsFalse)
{
    DateSectionParams p;
    p.mimeType = MIME_TYPE_PHOTO;
    p.order = OrderByClause::MTIME_DESC;
    p.granularity = DateSectionGranularity::Month;

    std::vector<DateSection> out;
    EXPECT_FALSE(tableNodes()->groupAllNodesByDate(p, /*filesRoots=*/{}, out, CancelToken{}));
    EXPECT_TRUE(out.empty());
}

TEST_F(DateSectionTest, DateSection_InvalidMimeType_ReturnsFalse)
{
    DateSectionParams p;
    p.mimeType = MIME_TYPE_UNKNOWN;
    p.order = OrderByClause::MTIME_DESC;
    p.granularity = DateSectionGranularity::Month;

    std::vector<DateSection> out;
    EXPECT_FALSE(tableNodes()->groupAllNodesByDate(p, {mDateSectionRoot}, out, CancelToken{}));
    EXPECT_TRUE(out.empty());
}

TEST_F(DateSectionTest, DateSection_OutOfRangeMimeType_ReturnsFalse)
{
    // The API layer caps mimeType at FILE_TYPE_LAST, but a direct DB caller could pass
    // an out-of-range value. The DB entry must reject it (it is the cache-key's leading
    // digit) rather than packing an out-of-range digit deeper in computeDateSectionsCacheId.
    DateSectionParams p;
    p.mimeType = static_cast<MimeType_t>(MIME_TYPE_ALL_VISUAL_MEDIA + 1);
    p.order = OrderByClause::MTIME_DESC;
    p.granularity = DateSectionGranularity::Month;

    std::vector<DateSection> out;
    EXPECT_FALSE(tableNodes()->groupAllNodesByDate(p, {mDateSectionRoot}, out, CancelToken{}));
    EXPECT_TRUE(out.empty());
}

TEST_F(DateSectionTest, DateSection_OutOfRangeGranularity_ReturnsFalse)
{
    // The API layer caps granularity to [Day, Year], but a direct DB caller could cast an
    // out-of-range value. The DB entry must reject it (it is a cache-key digit) rather than
    // relying on the assert/fallback in buildDateSectionGidExpr / buildDateSectionBoundExprs.
    DateSectionParams p;
    p.mimeType = MIME_TYPE_PHOTO;
    p.order = OrderByClause::MTIME_DESC;
    p.granularity =
        static_cast<DateSectionGranularity>(static_cast<int>(DateSectionGranularity::Year) + 1);

    std::vector<DateSection> out;
    EXPECT_FALSE(tableNodes()->groupAllNodesByDate(p, {mDateSectionRoot}, out, CancelToken{}));
    EXPECT_TRUE(out.empty());
}

TEST_F(DateSectionTest, DateSection_UnsupportedOrder_ReturnsFalse)
{
    // A non-timestamp order (DEFAULT_ASC) has no timestamp column, so the DB
    // entry point must reject it up front rather than failing deep in prepare.
    DateSectionParams p;
    p.mimeType = MIME_TYPE_PHOTO;
    p.order = OrderByClause::DEFAULT_ASC;
    p.granularity = DateSectionGranularity::Month;

    std::vector<DateSection> out;
    EXPECT_FALSE(tableNodes()->groupAllNodesByDate(p, {mDateSectionRoot}, out, CancelToken{}));
    EXPECT_TRUE(out.empty());
}

TEST_F(DateSectionTest, DateSection_FileVersions_Excluded)
{
    // Versioned files come from the base SearchByPageTest fixture (head.jpg
    // chain under normal_folder). Scope to the cloud root to include them in
    // the candidate set, then confirm they don't appear in the section list.
    const auto v = runSections(DateSectionGranularity::Month,
                               OrderByClause::MTIME_DESC,
                               MIME_TYPE_PHOTO,
                               /*excludeSensitive=*/false,
                               /*excludeHandles=*/{},
                               /*roots=*/{hFilesRoot});

    // The base fixture's hVersionV1 / hVersionV2 are FILENODEs flagged as
    // versions and must never appear. Total = base non-version photos + photos
    // added here; named constants keep the count tied to the fixture.
    constexpr int64_t kBaseNonVersionPhotos = 4; // clean, self_sens, head HEAD, under_sens
    constexpr int64_t kDateSectionPhotos = 11; // see DateSection::SetUp() photos table
    constexpr int64_t kExpectedTotal = kBaseNonVersionPhotos + kDateSectionPhotos;
    int64_t total = 0;
    for (const auto& s: v)
        total += s.mCount;
    EXPECT_EQ(total, kExpectedTotal);
}

// ─── B5: grouped mime types ───────────────────────────────────────────────

TEST_F(DateSectionTest, DateSection_AllVisualMedia_AggregatesPhotosVideos)
{
    // July: 5 photos + 2 videos = 7 in 2024-07 bucket.
    const auto v = runSections(DateSectionGranularity::Month,
                               OrderByClause::MTIME_DESC,
                               MIME_TYPE_ALL_VISUAL_MEDIA);

    auto* jul = find(v, "2024-07");
    ASSERT_NE(jul, nullptr);
    EXPECT_EQ(jul->mCount, 7);
    EXPECT_EQ(jul->mStartDate, 1719792000);
    EXPECT_EQ(jul->mEndDate, 1722470400);
}

TEST_F(DateSectionTest, DateSection_AllVisualMedia_PerRouteAggregationConsistent)
{
    // sum(buckets in grouped) == sum(photos) + sum(videos)
    const auto photos =
        runSections(DateSectionGranularity::Year, OrderByClause::MTIME_DESC, MIME_TYPE_PHOTO);
    const auto videos =
        runSections(DateSectionGranularity::Year, OrderByClause::MTIME_DESC, MIME_TYPE_VIDEO);
    const auto grouped = runSections(DateSectionGranularity::Year,
                                     OrderByClause::MTIME_DESC,
                                     MIME_TYPE_ALL_VISUAL_MEDIA);

    auto sumOf = [](const std::vector<DateSection>& v)
    {
        int64_t s = 0;
        for (const auto& d: v)
            s += d.mCount;
        return s;
    };
    EXPECT_EQ(sumOf(grouped), sumOf(photos) + sumOf(videos));
}

TEST_F(DateSectionTest, DateSection_AllVisualMedia_BoundsMatchSimpleMime)
{
    const auto photos =
        runSections(DateSectionGranularity::Month, OrderByClause::MTIME_DESC, MIME_TYPE_PHOTO);
    const auto grouped = runSections(DateSectionGranularity::Month,
                                     OrderByClause::MTIME_DESC,
                                     MIME_TYPE_ALL_VISUAL_MEDIA);

    auto* photosJul = find(photos, "2024-07");
    auto* groupedJul = find(grouped, "2024-07");
    ASSERT_NE(photosJul, nullptr);
    ASSERT_NE(groupedJul, nullptr);

    EXPECT_EQ(photosJul->mStartDate, groupedJul->mStartDate);
    EXPECT_EQ(photosJul->mEndDate, groupedJul->mEndDate);
}

// ─── B6: byTimestampAnchor half-bounded clause on listAllNodesByPage ──────

namespace
{
TimestampAnchorFilter julyMtimeAnchorAsc()
{
    TimestampAnchorFilter ta;
    ta.mOrder = OrderByClause::MTIME_ASC; // ASC anchor → enforce mtime >= startSec
    ta.mStartSeconds = 1719792000; // 2024-07-01 UTC inclusive
    ta.mEndSeconds = 1722470400; // 2024-08-01 UTC exclusive
    return ta;
}

TimestampAnchorFilter julyMtimeAnchorDesc()
{
    TimestampAnchorFilter ta;
    ta.mOrder = OrderByClause::MTIME_DESC; // DESC anchor → enforce mtime < endSec
    ta.mStartSeconds = 1719792000;
    ta.mEndSeconds = 1722470400;
    return ta;
}
} // namespace

TEST_F(DateSectionTest, ListAllByPage_AnchorAsc_LowerBoundEnforced)
{
    // ASC anchor → mtime >= startSec (1719792000). Excludes jun01 / feb29 / jan01 / dec31_2023.
    const auto rows = runListAll(OrderByClause::MTIME_ASC, julyMtimeAnchorAsc());

    EXPECT_TRUE(hasMtime(rows, 1719792000)); // jul01 (== start, inclusive)
    EXPECT_TRUE(hasMtime(rows, 1722470400)); // aug01 (>= start, end is NOT enforced)
    EXPECT_TRUE(hasMtime(rows, 1735603200)); // dec31_2024 (later, included)
    EXPECT_FALSE(hasMtime(rows, 1717200000)); // jun01 — before start
    EXPECT_FALSE(hasMtime(rows, 1704067200)); // jan01 — before start
}

TEST_F(DateSectionTest, ListAllByPage_AnchorDesc_UpperBoundEnforced)
{
    // DESC anchor → mtime < endSec (1722470400). Excludes aug01 / dec31_2024.
    const auto rows = runListAll(OrderByClause::MTIME_DESC, julyMtimeAnchorDesc());

    EXPECT_TRUE(hasMtime(rows, 1722470399)); // jul31 (< end)
    EXPECT_TRUE(hasMtime(rows, 1717200000)); // jun01 (older, included — start NOT enforced)
    EXPECT_TRUE(hasMtime(rows, 1704067200)); // jan01 (older still)
    EXPECT_FALSE(hasMtime(rows, 1722470400)); // aug01 — exactly at end, excluded
    EXPECT_FALSE(hasMtime(rows, 1735603200)); // dec31_2024 — after end
}

TEST_F(DateSectionTest, ListAllByPage_AnchorAsc_StartInclusive)
{
    // jul01.jpg's mtime == startSec; must be included with ASC anchor.
    const auto rows = runListAll(OrderByClause::MTIME_ASC, julyMtimeAnchorAsc());
    EXPECT_TRUE(hasMtime(rows, 1719792000)); // jul01
}

TEST_F(DateSectionTest, ListAllByPage_AnchorDesc_EndExclusive)
{
    // aug01.jpg's mtime == endSec; must be excluded with DESC anchor.
    const auto rows = runListAll(OrderByClause::MTIME_DESC, julyMtimeAnchorDesc());
    EXPECT_FALSE(hasMtime(rows, 1722470400));
}

TEST_F(DateSectionTest, ListAllByPage_AnchorAsc_Mtime0_Excluded)
{
    // ASC anchor at start=0: the anchor path's `> invalidSentinel` guard
    // excludes epoch.jpg (mtime=0) and negMtime.jpg (mtime=-1) — they're in
    // no section, so anchored pages must not surface them.
    TimestampAnchorFilter ta;
    ta.mOrder = OrderByClause::MTIME_ASC;
    ta.mStartSeconds = 0;
    ta.mEndSeconds = 1735689600;
    const auto rows = runListAll(OrderByClause::MTIME_ASC, ta);

    EXPECT_FALSE(hasMtime(rows, 0));
    EXPECT_FALSE(hasMtime(rows, -1));
}

TEST_F(DateSectionTest, ListAllByPage_AnchorDesc_Mtime0_Excluded)
{
    // Regression: a DESC anchor (`< end`) has no lower bound, so without the
    // `> invalidSentinel` guard mtime=0 / -1 nodes would leak into the tail —
    // even though no section counts them. Must be excluded like the ASC case.
    TimestampAnchorFilter ta;
    ta.mOrder = OrderByClause::MTIME_DESC;
    ta.mStartSeconds = 0;
    ta.mEndSeconds = 1735689600; // after every real node, so the tail is reached
    const auto rows = runListAll(OrderByClause::MTIME_DESC, ta);

    EXPECT_FALSE(hasMtime(rows, 0));
    EXPECT_FALSE(hasMtime(rows, -1));
}

TEST_F(DateSectionTest, ListAllByPage_Anchor_OrthogonalToPageOrder)
{
    // DESC anchor (mtime < endSec) + page order = SIZE_DESC (ORDER BY size DESC).
    // Pins the orthogonality contract: anchor's sectionOrder controls the
    // half-bound; page order controls only ORDER BY. The two CAN be different
    // columns / directions and the SQL composes cleanly.
    const auto rows = runListAll(OrderByClause::SIZE_DESC, julyMtimeAnchorDesc());

    // Half-bound stays in effect even when page order isn't a timestamp.
    EXPECT_FALSE(hasMtime(rows, 1722470400)); // aug01 (mtime == endSec)
    EXPECT_FALSE(hasMtime(rows, 1735603200)); // dec31_2024 (after endSec)
    EXPECT_TRUE(hasMtime(rows, 1717200000)); // jun01 (start NOT enforced)

    // Sorted by size DESC.
    const auto rowSizes = sizes(rows);
    for (size_t i = 1; i < rowSizes.size(); ++i)
        EXPECT_LE(rowSizes[i], rowSizes[i - 1]);
}

TEST_F(DateSectionTest, ListAllByPage_Anchor_DirectionFromSectionOrder_NotPageOrder)
{
    // sectionOrder=DESC sets the half-bound (mtime<end) independent of page
    // order=ASC (ORDER BY).
    const auto rows = runListAll(OrderByClause::MTIME_ASC, julyMtimeAnchorDesc());

    EXPECT_TRUE(hasMtime(rows, 1704067200)); // jan01 (< endSec, included)
    EXPECT_TRUE(hasMtime(rows, 1717200000)); // jun01 (< endSec, included)
    EXPECT_TRUE(hasMtime(rows, 1722470399)); // jul31 (< endSec, included)
    EXPECT_FALSE(hasMtime(rows, 1722470400)); // aug01 (== endSec, excluded)
    EXPECT_FALSE(hasMtime(rows, 1735603200)); // dec31_2024 (> endSec, excluded)

    // ORDER BY mtime ASC: each successive row has a non-decreasing mtime.
    const auto rowMtimes = mtimes(rows);
    for (size_t i = 1; i < rowMtimes.size(); ++i)
        EXPECT_LE(rowMtimes[i - 1], rowMtimes[i]);
}

TEST_F(DateSectionTest, ListAllByPage_Anchor_PagesCrossSectionBoundary)
{
    // Small page (3) + DESC anchor on July → first page should yield 3
    // newest-mtime photos with mtime < endSec, all from 2024-07 (5 July
    // photos available, none from August in the result).
    const auto rows = runListAll(OrderByClause::MTIME_DESC,
                                 julyMtimeAnchorDesc(),
                                 /*maxElements=*/3);
    ASSERT_EQ(rows.size(), 3u);

    const auto rowMtimes = mtimes(rows);
    // All three should be within July (mtime in [1719792000, 1722470400)).
    for (int64_t mt: rowMtimes)
    {
        EXPECT_GE(mt, 1719792000);
        EXPECT_LT(mt, 1722470400);
    }

    // Newest first (DESC) — strict descending.
    EXPECT_GT(rowMtimes[0], rowMtimes[1]);
    EXPECT_GT(rowMtimes[1], rowMtimes[2]);
}

} // anonymous namespace

#endif // USE_SQLITE
