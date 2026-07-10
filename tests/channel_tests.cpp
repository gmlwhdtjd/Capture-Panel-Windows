#include "test_framework.hpp"

#include "capture_panel/core/channels.hpp"
#include "capture_panel/core/errors.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

using namespace capture_panel;

namespace {

[[nodiscard]] bool parsing_fails(const std::string_view specification) {
    try {
        static_cast<void>(parse_channel_spec(specification));
        return false;
    } catch (const CaptureError& error) {
        return error.code() == ErrorCode::invalid_channel_specification;
    }
}

} // namespace

CP_TEST_CASE("channel parser reads one-based channel lists") {
    CP_REQUIRE(parse_channel_spec("1") == std::vector<std::uint32_t>{1});
    CP_REQUIRE((parse_channel_spec("1,2,3") == std::vector<std::uint32_t>{1, 2, 3}));
    CP_REQUIRE((parse_channel_spec(" 1, 2 , 3 ") == std::vector<std::uint32_t>{1, 2, 3}));
}

CP_TEST_CASE("channel parser expands ranges and preserves order") {
    CP_REQUIRE((parse_channel_spec("1-4") == std::vector<std::uint32_t>{1, 2, 3, 4}));
    CP_REQUIRE(parse_channel_spec("3-3") == std::vector<std::uint32_t>{3});
    CP_REQUIRE((parse_channel_spec("1,3-5,8") == std::vector<std::uint32_t>{1, 3, 4, 5, 8}));
    CP_REQUIRE((parse_channel_spec("2, 4 - 5, 2") == std::vector<std::uint32_t>{2, 4, 5, 2}));
}

CP_TEST_CASE("channel parser rejects malformed specifications") {
    CP_REQUIRE(parsing_fails(""));
    CP_REQUIRE(parsing_fails("   "));
    CP_REQUIRE(parsing_fails("0"));
    CP_REQUIRE(parsing_fails("-1"));
    CP_REQUIRE(parsing_fails("abc"));
    CP_REQUIRE(parsing_fails("5-2"));
    CP_REQUIRE(parsing_fails("1-2-3"));
    CP_REQUIRE(parsing_fails("1,,2"));
    CP_REQUIRE(parsing_fails("1,"));
    CP_REQUIRE(parsing_fails("4294967296"));
}

CP_TEST_CASE("channel validation accepts channels exposed by a device") {
    const std::vector<std::uint32_t> playback{1, 2};
    const std::vector<std::uint32_t> record{2, 4};
    validate_playback_channels(playback, 2);
    validate_record_channels(record, 4);
    CP_REQUIRE(true);
}

CP_TEST_CASE("channel validation rejects zero, empty, and out-of-range channels") {
    for (const auto& channels : std::vector<std::vector<std::uint32_t>>{{}, {0}, {1, 3}}) {
        bool failed = false;
        try {
            validate_playback_channels(channels, 2);
        } catch (const CaptureError& error) {
            failed = error.code() == ErrorCode::validation_failed;
        }
        CP_REQUIRE(failed);
    }
}
