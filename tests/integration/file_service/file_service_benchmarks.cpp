#include <mega/common/testing/single_client_test.h>
#include <mega/common/testing/statistics.h>
#include <mega/file_service/testing/integration/client.h>
#include <mega/file_service/testing/integration/real_client.h>

namespace mega
{
namespace file_service
{
namespace testing
{

using common::testing::Averager;
using common::testing::Maximizer;
using common::testing::Minimizer;
using common::testing::SingleClientTest;

struct FileServiceBenchmarkTraits
{
    using AbstractClient = Client;
    using ConcreteClient = RealClient;

    static constexpr const char* mName = "file_service_benchmark";
}; // FileServiceBenchmarkTraits

struct FileServiceBenchmark: SingleClientTest<FileServiceBenchmarkTraits>
{}; // FileServiceBenchmark

} // testing
} // file_service
} // mega
