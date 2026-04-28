#include "easy_curl.h"
#include "mega/common/testing/utility.h"
#include "mega/utils.h"
#include "sdk_server_test_utils.h"

using ::mega::common::testing::randomBytes;
using ::mega::common::testing::randomName;

class SdkFileServiceTest: public SdkServerTest
{
protected:
    void SetUp() override;

    void TearDown() override;

    std::string mFileContent;

    std::string mFileName;

    size_t mFileSize{5 * 1024};

    std::optional<ScopedDestructor> mHttpServer;
};

void SdkFileServiceTest::SetUp()
{
    SdkServerTest::SetUp();

    mFileContent = randomBytes(mFileSize);

    mFileName = randomName();

    ASSERT_NO_FATAL_FAILURE(getAccountsForTest(1));

    MegaApi* api = megaApi[0].get();

    std::unique_ptr<MegaNode> uploadedNode = uploadFile(0, mFileName, mFileContent);
    ASSERT_NE(uploadedNode, nullptr);

    mHttpServer = scopedHttpServer(api);
    ASSERT_TRUE(mHttpServer);

    std::unique_ptr<char[]> link(api->httpServerGetLocalLink(uploadedNode.get()));
    ASSERT_NE(link, nullptr);
    std::string url = link.get();

    auto response = HttpClient::get(url);
    EXPECT_EQ(200, response.statusCode);
    EXPECT_EQ(mFileContent, response.body);
}

void SdkFileServiceTest::TearDown()
{
    SdkServerTest::TearDown();
}

TEST_F(SdkFileServiceTest, FileServiceGetStorageInfoSuccessfully)
{
    MegaApi* api = megaApi[0].get();

    // Get storage info using file service's current reclaim options
    std::unique_ptr<MegaFileServiceStorageInfo> info{api->fileServiceGetStorageInfo(nullptr)};
    ASSERT_TRUE(info != nullptr);
    ASSERT_GE(info->getAllocatedSize(), mFileSize);
    ASSERT_EQ(info->getReclaimableSize(), 0);

    // Get storage info using input reclaim options
    std::unique_ptr<MegaFileServiceReclaimOptions> options{MegaFileServiceReclaimOptions::create()};
    options->setReclaimTarget(0);
    options->setReclaimThreshold(0);
    options->setAgeThreshold(0);
    info.reset(api->fileServiceGetStorageInfo(options.get()));
    ASSERT_GE(info->getAllocatedSize(), mFileSize);
    ASSERT_EQ(info->getReclaimableSize(), mFileSize);
}

TEST_F(SdkFileServiceTest, FileServiceReclaimSuccessfully)
{
    MegaApi* api = megaApi[0].get();

    // Create a reclaim options object to claim all storages
    std::unique_ptr<MegaFileServiceReclaimOptions> options{MegaFileServiceReclaimOptions::create()};
    options->setReclaimTarget(0);
    options->setReclaimThreshold(0);
    options->setAgeThreshold(0);

    // Reclaim it
    RequestTracker rt(api);
    api->fileServiceReclaim(options.get(), &rt);
    ASSERT_EQ(API_OK, rt.waitForResult());

    // No storage afterwards
    std::unique_ptr<MegaFileServiceStorageInfo> info{api->fileServiceGetStorageInfo(nullptr)};
    ASSERT_TRUE(info != nullptr);
    ASSERT_GE(info->getAllocatedSize(), 0);
    ASSERT_EQ(info->getReclaimableSize(), 0);
}
