#include <gtest/gtest.h>
#include <mega/common/database.h>
#include <mega/common/subsystem_logger.h>
#include <mega/file_service/database_builder.h>
#include <mega/localpath.h>

namespace mega
{
namespace file_service
{
namespace testing
{

using common::Database;
using common::SubsystemLogger;
using ::mega::file_service::DatabaseBuilder;

SubsystemLogger& logger()
{
    static SubsystemLogger logger("FileServiceDatabaseBuilderTest");

    return logger;
}

TEST(FileServiceDatabaseBuilder, CreateAndDowngrade)
{
    Database database(logger(),
                      mega::LocalPath::fromRelativePath("file_service_create_and_downgrade.db"));

    // Create
    ASSERT_NO_THROW(DatabaseBuilder(database).build());

    // Downgrade
    ASSERT_NO_THROW(DatabaseBuilder(database).downgrade(0));

    // Upgrade
    ASSERT_NO_THROW(DatabaseBuilder(database).upgrade(std::numeric_limits<std::size_t>::max()));
}

} // testing
} // file_service
} // mega
