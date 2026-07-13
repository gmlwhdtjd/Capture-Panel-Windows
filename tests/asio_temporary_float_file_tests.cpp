#include "asio_temporary_float_file.hpp"
#include "test_framework.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

namespace capture_panel::test {
namespace {

[[nodiscard]] std::filesystem::path scratch_prefix(const std::string& name) {
    return std::filesystem::temp_directory_path()
        / ("CapturePanel-" + name + ".capture-panel.tmp.");
}

} // namespace

CP_TEST_CASE("ASIO temporary Float32 files use the cleanup-compatible prefix") {
    const auto prefix = scratch_prefix("prefix");
    std::filesystem::path first_path;
    std::filesystem::path second_path;
    {
        asio::AsioTemporaryFloatFile first(prefix);
        asio::AsioTemporaryFloatFile second(prefix);
        first_path = first.path();
        second_path = second.path();
        CP_REQUIRE(first_path != second_path);
        CP_REQUIRE(first_path.native().starts_with(prefix.native()));
        CP_REQUIRE(second_path.native().starts_with(prefix.native()));
        CP_REQUIRE(std::filesystem::exists(first_path));
        CP_REQUIRE(std::filesystem::exists(second_path));
    }
    CP_REQUIRE(!std::filesystem::exists(first_path));
    CP_REQUIRE(!std::filesystem::exists(second_path));
}

CP_TEST_CASE("ASIO temporary Float32 file transfers only a closed complete file") {
    const auto prefix = scratch_prefix("transfer");
    std::filesystem::path path;
    {
        asio::AsioTemporaryFloatFile file(prefix);
        const std::array samples{1.0F, -0.5F, 0.25F};
        file.write(samples);
        file.close();
        path = file.release_ownership();
    }
    CP_REQUIRE(std::filesystem::exists(path));
    CP_REQUIRE(std::filesystem::file_size(path) == 3 * sizeof(float));

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

CP_TEST_CASE("ASIO temporary Float32 file falls back for a long path component") {
    const auto prefix = std::filesystem::temp_directory_path()
        / std::string(250, 'x');
    std::filesystem::path path;
    {
        asio::AsioTemporaryFloatFile file(prefix);
        path = file.path();
        CP_REQUIRE(path.filename().native().starts_with(L".capture-panel.tmp."));
        CP_REQUIRE(std::filesystem::exists(path));
    }
    CP_REQUIRE(!std::filesystem::exists(path));
}

} // namespace capture_panel::test
