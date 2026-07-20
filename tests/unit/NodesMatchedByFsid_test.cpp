/**
 * @file NodesMatchedByFsid_test.cpp
 * @brief Tests for the methods used to search for nodes matched by fsid.
 */

#ifdef ENABLE_SYNC

#include <gtest/gtest.h>
#include <mega/syncinternals/syncinternals.h>

using namespace mega;

namespace
{

/**
 * @brief A default value for the user owner handle.
 */
constexpr handle COMMON_USER_OWNER = 1;
/**
 * @brief A default value for the isFsidReused flag.
 */
constexpr bool FSID_REUSED = false;
/**
 * @brief A default SourceNodeMatchByFSIDContext struct.
 */
constexpr SourceNodeMatchByFSIDContext BASIC_SOURCE_CONTEXT{FSID_REUSED,
                                                            ExclusionState::ES_INCLUDED};
/**
 * @brief A default mtime.
 */
constexpr m_time_t SIMPLE_MTIME = 1;
/**
 * @brief A default size.
 */
constexpr m_off_t SIMPLE_SIZE = 10;

/**
 * @brief Generates a light FileFingerprint (mtime and size).
 *
 * This light FileFingerprint is enough for comparison purposes,
 * the CRC needs real data to be calculated, and we are not
 * testing FileFingerprint fields here.
 *
 * @param mtime The modification time of the file.
 * @param size The size of the file.
 * @return A valid FileFingerprint with the mtime and size. CRC would be empty.
 */
FileFingerprint genLightFingerprint(const m_time_t mtime = SIMPLE_MTIME,
                                    const m_off_t size = SIMPLE_SIZE)
{
    FileFingerprint lightFp{};
    lightFp.mtime = mtime;
    lightFp.size = size;
    lightFp.isvalid = true;
    return lightFp;
}

/**
 * @brief Owns the data referenced by NodeMatchByFSIDAttributes.
 *
 * NodeMatchByFSIDAttributes stores references to fsfp and fingerprints, so the
 * underlying objects must outlive any comparison call.
 */
struct MatchAttributesFixture
{
    fsfp_t mFsfp;
    FileFingerprint mFingerprint;
    FileFingerprint mRealFingerprint;
    NodeMatchByFSIDAttributes mAttributes;

    MatchAttributesFixture(const nodetype_t nodeType = FILENODE,
                           fsfp_t fsfp = {1, "UUID"},
                           const handle userHandle = COMMON_USER_OWNER,
                           FileFingerprint fingerprint = genLightFingerprint(),
                           FileFingerprint realFingerprint = genLightFingerprint()):
        mFsfp(std::move(fsfp)),
        mFingerprint(std::move(fingerprint)),
        mRealFingerprint(std::move(realFingerprint)),
        mAttributes{nodeType, mFsfp, userHandle, mFingerprint, mRealFingerprint}
    {}
};

} // namespace

/**
 * @brief Tests a match: both nodes are equivalent.
 */
TEST(NodesMatchedByFSIDTest, NodesAreEquivalent)
{
    const MatchAttributesFixture source;
    const MatchAttributesFixture target;

    ASSERT_EQ(areNodesMatchedByFsidEquivalent(source.mAttributes,
                                              target.mAttributes,
                                              BASIC_SOURCE_CONTEXT),
              NodeMatchByFSIDResult::Matched);
}

/**
 * @brief Tests mismatch due to FSID reused by the source node.
 */
TEST(NodesMatchedByFSIDTest, SourceNodeFsidReused)
{
    const MatchAttributesFixture source;
    const MatchAttributesFixture target;

    constexpr bool fsidIsReused = true;
    constexpr SourceNodeMatchByFSIDContext context{fsidIsReused, ExclusionState::ES_INCLUDED};

    ASSERT_EQ(areNodesMatchedByFsidEquivalent(source.mAttributes, target.mAttributes, context),
              NodeMatchByFSIDResult::SourceFsidReused);
}

/**
 * @brief Test mismatch due to different filesystem fingerprints.
 */
TEST(NodesMatchedByFSIDTest, DifferentFilesystemsFingerprints)
{
    const fsfp_t fsfp1{1, "UUID"};
    const fsfp_t fsfp2{2, "UUID2"};
    const MatchAttributesFixture source(FILENODE, fsfp1);
    const MatchAttributesFixture target(FILENODE, fsfp2);

    ASSERT_EQ(areNodesMatchedByFsidEquivalent(source.mAttributes,
                                              target.mAttributes,
                                              BASIC_SOURCE_CONTEXT),
              NodeMatchByFSIDResult::DifferentFilesystems);
}

/**
 * @brief Tests mismatch due to different node types.
 */
TEST(NodesMatchedByFSIDTest, DifferentNodeTypes)
{
    const MatchAttributesFixture source(FILENODE);
    const MatchAttributesFixture target(FOLDERNODE);

    ASSERT_EQ(areNodesMatchedByFsidEquivalent(source.mAttributes,
                                              target.mAttributes,
                                              BASIC_SOURCE_CONTEXT),
              NodeMatchByFSIDResult::DifferentTypes);
}

/**
 * @brief Tests mismatch due to different owners.
 */
TEST(NodesMatchedByFSIDTest, DifferentOwners)
{
    constexpr handle sourceOwner = 1;
    constexpr handle targetOwner = 2;

    const fsfp_t fsfp1{1, "UUID"};
    const MatchAttributesFixture source(FILENODE, fsfp1, sourceOwner);
    const MatchAttributesFixture target(FILENODE, fsfp1, targetOwner);

    ASSERT_EQ(areNodesMatchedByFsidEquivalent(source.mAttributes,
                                              target.mAttributes,
                                              BASIC_SOURCE_CONTEXT),
              NodeMatchByFSIDResult::DifferentOwners);
}

/**
 * @brief Tests mismatch due to exclusion unknown.
 */
TEST(NodesMatchedByFSIDTest, SourceNodeExclusionStateIsUnknown)
{
    const MatchAttributesFixture source;
    const MatchAttributesFixture target;

    constexpr SourceNodeMatchByFSIDContext context{false, ExclusionState::ES_UNKNOWN};

    ASSERT_EQ(areNodesMatchedByFsidEquivalent(source.mAttributes, target.mAttributes, context),
              NodeMatchByFSIDResult::SourceExclusionUnknown);
}

/**
 * @brief Tests mismatch due to node exclusion.
 */
TEST(NodesMatchedByFSIDTest, SourceNodeIsExcluded)
{
    const MatchAttributesFixture source;
    const MatchAttributesFixture target;

    constexpr SourceNodeMatchByFSIDContext context{false, ExclusionState::ES_EXCLUDED};

    ASSERT_EQ(areNodesMatchedByFsidEquivalent(source.mAttributes, target.mAttributes, context),
              NodeMatchByFSIDResult::SourceIsExcluded);
}

/**
 * @brief Test mismatch due to different fingerprint due to mtime.
 */
TEST(NodesMatchedByFSIDTest, DifferentFingerprintDueToMtime)
{
    const auto sourceFp = genLightFingerprint();
    const auto targetFp = genLightFingerprint(SIMPLE_MTIME + 30, SIMPLE_SIZE);

    const fsfp_t fsfp1{1, "UUID"};
    const MatchAttributesFixture source(FILENODE, fsfp1, COMMON_USER_OWNER, sourceFp);
    const MatchAttributesFixture target(FILENODE, fsfp1, COMMON_USER_OWNER, targetFp);

    ASSERT_EQ(areNodesMatchedByFsidEquivalent(source.mAttributes,
                                              target.mAttributes,
                                              BASIC_SOURCE_CONTEXT),
              NodeMatchByFSIDResult::DifferentFingerprintOnlyMtime);
}

/**
 * @brief Test mismatch due to different fingerprint due to size.
 */
TEST(NodesMatchedByFSIDTest, DifferentFingerprintDueToSize)
{
    const auto sourceFp = genLightFingerprint();
    const auto targetFp = genLightFingerprint(SIMPLE_MTIME, SIMPLE_SIZE + 1);

    const fsfp_t fsfp1{1, "UUID"};
    const MatchAttributesFixture source(FILENODE, fsfp1, COMMON_USER_OWNER, sourceFp);
    const MatchAttributesFixture target(FILENODE, fsfp1, COMMON_USER_OWNER, targetFp);

    ASSERT_EQ(areNodesMatchedByFsidEquivalent(source.mAttributes,
                                              target.mAttributes,
                                              BASIC_SOURCE_CONTEXT),
              NodeMatchByFSIDResult::DifferentFingerprint);
}

#endif // ENABLE_SYNC