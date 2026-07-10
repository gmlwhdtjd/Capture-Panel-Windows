#include "asio_driver_registry.hpp"
#include "test_framework.hpp"

#include <string>

namespace capture_panel::test {
namespace {

constexpr auto sample_clsid = "{AC46741D-EF45-46E6-920B-54608666926A}";

CP_TEST_CASE("ASIO CLSID canonicalization accepts common registry forms") {
    const auto braced = asio::canonicalize_clsid(
        "  {ac46741d-ef45-46e6-920b-54608666926a}\r\n");
    const auto unbraced = asio::canonicalize_clsid(
        "ac46741d-ef45-46e6-920b-54608666926a");

    CP_REQUIRE(braced.has_value());
    CP_REQUIRE(*braced == sample_clsid);
    CP_REQUIRE(unbraced.has_value());
    CP_REQUIRE(*unbraced == sample_clsid);
}

CP_TEST_CASE("ASIO CLSID canonicalization rejects malformed values") {
    CP_REQUIRE(!asio::canonicalize_clsid("").has_value());
    CP_REQUIRE(!asio::canonicalize_clsid("{AC46741D-EF45-46E6-920B-54608666926}").has_value());
    CP_REQUIRE(!asio::canonicalize_clsid("AC46741D-EF45-46E6-920B54608666926A").has_value());
    CP_REQUIRE(!asio::canonicalize_clsid("AC46741D-EF45-46E6-920B-54608666926Z").has_value());
    CP_REQUIRE(!asio::canonicalize_clsid("[AC46741D-EF45-46E6-920B-54608666926A]").has_value());
}

CP_TEST_CASE("ASIO device ids round trip through a canonical CLSID") {
    const auto id = asio::make_asio_device_id(
        "ac46741d-ef45-46e6-920b-54608666926a");
    CP_REQUIRE(id.has_value());
    CP_REQUIRE(*id == std::string("asio:") + sample_clsid);

    const auto parsed = asio::parse_asio_device_id(*id);
    CP_REQUIRE(parsed.has_value());
    CP_REQUIRE(*parsed == sample_clsid);
}

CP_TEST_CASE("ASIO device id parser requires the ASIO namespace") {
    CP_REQUIRE(!asio::parse_asio_device_id(sample_clsid).has_value());
    CP_REQUIRE(!asio::parse_asio_device_id(std::string("ASIO:") + sample_clsid).has_value());
    CP_REQUIRE(!asio::parse_asio_device_id("asio:not-a-guid").has_value());
    CP_REQUIRE(!asio::make_asio_device_id("not-a-guid").has_value());
}

} // namespace
} // namespace capture_panel::test
