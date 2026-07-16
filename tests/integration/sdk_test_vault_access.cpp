/**
 * @file
 * @brief Integration tests for MegaApi::getAccess() over the Vault (SDK-6328).
 *
 * Behaviour under test:
 *   - The Vault is read-only for client apps: getAccess() returns ACCESS_READ for
 *     the Vault root and for every non-password descendant (e.g. the "My Backups"
 *     folder), at any depth.
 *   - The Password Manager subtree is the only exception.
 *   - Nodes outside the Vault (Cloud Drive and its children) are unaffected.
 *
 * Two fixtures are required because no single client type loads every node needed:
 *   - SdkTestVaultAccess (DEFAULT client) loads the Cloud Drive, Rubbish and Vault
 *     roots, so it covers the Vault root, "My Backups" and the Cloud Drive cases.
 *   - SdkTestVaultAccessPasswordManager (PASSWORD_MANAGER client) auto-creates the
 *     Password Manager base and loads the Vault root, so it covers the
 *     password-subtree exception while the read-only branch is genuinely exercised
 *     (firstancestor() reaches the loaded VAULTNODE).
 */

#include "integration_test_utils.h"
#include "mock_listeners.h"
#include "passwordManager/SdkTestPasswordManager.h"

#include <gmock/gmock.h>

using namespace testing;

class SdkTestVaultAccess: public SdkTest
{};

TEST_F(SdkTestVaultAccess, VaultIsReadOnlyCloudDriveIsNot)
{
    ASSERT_NO_FATAL_FAILURE(getAccountsForTest(1));
    MegaApi* const api = megaApi[0].get();
    ASSERT_NE(api, nullptr);

    // The Vault root is read-only for the app.
    std::unique_ptr<MegaNode> vault{api->getVaultNode()};
    ASSERT_TRUE(vault) << "Vault node is not available";
    EXPECT_EQ(MegaShare::ACCESS_READ, api->getAccess(vault.get()))
        << "Vault root should be read-only for the app";

    // A non-password Vault descendant ("My Backups") is read-only too.
    ASSERT_TRUE(sdk_test::ensureMyBackupsFolderExists(api, "My backups").first)
        << "Could not ensure the My Backups folder exists";
    const auto [gotBackups, backupsHandle] = sdk_test::getMyBackupsFolder(api);
    ASSERT_TRUE(gotBackups);
    ASSERT_NE(backupsHandle, UNDEF);
    EXPECT_EQ(MegaShare::ACCESS_READ, api->getAccess(backupsHandle))
        << "My Backups folder (Vault descendant) should be read-only for the app";

    // Cloud Drive is outside the Vault and must keep owner access.
    std::unique_ptr<MegaNode> cloud{api->getRootNode()};
    ASSERT_TRUE(cloud) << "Cloud Drive root is not available";
    EXPECT_EQ(MegaShare::ACCESS_OWNER, api->getAccess(cloud.get()))
        << "Cloud Drive root should keep owner access";

    const MegaHandle folder = createFolder(0, "vault_access_regression", cloud.get());
    ASSERT_NE(folder, UNDEF);
    EXPECT_EQ(MegaShare::ACCESS_OWNER, api->getAccess(folder))
        << "A Cloud Drive folder should keep owner access";

    std::unique_ptr<MegaNode> folderNode{api->getNodeByHandle(folder)};
    if (folderNode)
    {
        ASSERT_EQ(API_OK, doDeleteNode(0, folderNode.get()));
    }

    // The Rubbish Bin is another root outside the Vault and must keep owner access.
    std::unique_ptr<MegaNode> rubbish{api->getRubbishNode()};
    ASSERT_TRUE(rubbish) << "Rubbish Bin root is not available";
    EXPECT_EQ(MegaShare::ACCESS_OWNER, api->getAccess(rubbish.get()))
        << "Rubbish Bin root should keep owner access";
}

class SdkTestVaultAccessPasswordManager: public SdkTestPasswordManager
{};

TEST_F(SdkTestVaultAccessPasswordManager, PasswordSubtreeKeepsOwnerAccess)
{
    // The Vault root is read-only even on a Password Manager client.
    std::unique_ptr<MegaNode> vault{mApi->getVaultNode()};
    ASSERT_TRUE(vault) << "Vault node is not available";
    EXPECT_EQ(MegaShare::ACCESS_READ, mApi->getAccess(vault.get()))
        << "Vault root should be read-only for the app";

    // The Password Manager base keeps its regular (owner) access.
    const std::unique_ptr<MegaNode> base = getBaseNode();
    ASSERT_TRUE(base) << "Password Manager base node is not available";
    EXPECT_EQ(MegaShare::ACCESS_OWNER, mApi->getAccess(base.get()))
        << "Password Manager base should keep owner access";

    // A password node under the base also keeps owner access.
    std::unique_ptr<MegaNode::PasswordNodeData> data{
        MegaNode::PasswordNodeData::createInstance("pwd", "notes", "url", "user", nullptr)};
    const auto pwdHandle =
        sdk_test::createPasswordNode(mApi, "vault_access_pwd", data.get(), getBaseHandle());
    ASSERT_NE(pwdHandle, UNDEF) << "Password node was not created";
    EXPECT_EQ(MegaShare::ACCESS_OWNER, mApi->getAccess(pwdHandle))
        << "A password node should keep owner access";
}
