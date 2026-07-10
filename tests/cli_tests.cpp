#include "test_framework.hpp"

#include "cli.hpp"
#include "capture_panel/core/diagnostics.hpp"
#include "capture_panel/core/wav.hpp"
#include "capture_panel/fake/fake_backend.hpp"

#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace capture_panel;

namespace {

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

[[nodiscard]] std::size_t count_occurrences(
    const std::string& text,
    std::string_view needle) {
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

struct TemporaryCliFiles {
    std::filesystem::path input = std::filesystem::temp_directory_path()
        / std::filesystem::path(u8"capture-panel-한글-source.wav");
    std::filesystem::path output = std::filesystem::temp_directory_path()
        / std::filesystem::path(u8"capture-panel-한글-result.wav");

    TemporaryCliFiles() {
        std::error_code ignored;
        std::filesystem::remove(input, ignored);
        std::filesystem::remove(output, ignored);
    }

    ~TemporaryCliFiles() {
        std::error_code ignored;
        std::filesystem::remove(input, ignored);
        std::filesystem::remove(output, ignored);
    }
};

} // namespace

CP_TEST_CASE("CLI parses the complete run command") {
    const auto command = cli::parse_arguments({
        "run",
        "--driver", "fake:loopback",
        "--play-channel", "1,3-4",
        "--record-channel=2-4",
        "--input", "source.wav",
        "--output=result.wav",
        "--bit-depth", "24",
        "--output-trim", "-6.5",
        "--verbose",
    });

    CP_REQUIRE(command.command == cli::Command::run);
    CP_REQUIRE(command.driver_id == "fake:loopback");
    CP_REQUIRE(command.playback_channels == std::vector<std::uint32_t>({1, 3, 4}));
    CP_REQUIRE(command.record_channels == std::vector<std::uint32_t>({2, 3, 4}));
    CP_REQUIRE(command.input_path == "source.wav");
    CP_REQUIRE(command.output_path == "result.wav");
    CP_REQUIRE(command.bit_depth == AudioBitDepth::pcm24);
    CP_REQUIRE_NEAR(command.output_trim_db, -6.5, 0.000001);
    CP_REQUIRE(command.verbose);
}

CP_TEST_CASE("CLI parses test routing and positional channels driver") {
    const auto test_command = cli::parse_arguments({
        "test",
        "--driver=fake:loopback",
        "--play-channel=1",
        "--record-channel=8",
    });
    CP_REQUIRE(test_command.command == cli::Command::test);
    CP_REQUIRE(test_command.record_channels == std::vector<std::uint32_t>({8}));

    const auto channels_command = cli::parse_arguments({"channels", "fake:loopback"});
    CP_REQUIRE(channels_command.command == cli::Command::channels);
    CP_REQUIRE(channels_command.driver_id == "fake:loopback");

    const auto input_devices = cli::parse_arguments({"devices", "-i"});
    CP_REQUIRE(input_devices.inputs_only);
    const auto output_devices = cli::parse_arguments({"devices", "-o"});
    CP_REQUIRE(output_devices.outputs_only);
}

CP_TEST_CASE("CLI rejects malformed or incomplete run options") {
    bool malformed_range = false;
    try {
        (void)cli::parse_arguments({
            "run", "--driver", "fake:loopback", "--play-channel", "3-1",
            "--record-channel", "1", "--input", "a.wav", "--output", "b.wav",
        });
    } catch (const std::invalid_argument&) {
        malformed_range = true;
    }
    CP_REQUIRE(malformed_range);

    bool missing_output = false;
    try {
        (void)cli::parse_arguments({
            "run", "--driver", "fake:loopback", "--play-channel", "1",
            "--record-channel", "1", "--input", "a.wav",
        });
    } catch (const std::invalid_argument&) {
        missing_output = true;
    }
    CP_REQUIRE(missing_output);
}

CP_TEST_CASE("CLI lists fake devices and channels through injected provider") {
    auto backend = std::make_shared<fake::FakeAudioBackend>();
    const cli::Dependencies dependencies{
        .device_provider = backend,
        .capture_backend = backend,
    };

    std::ostringstream device_output;
    std::ostringstream device_error;
    CP_REQUIRE(cli::run_cli({"devices"}, dependencies, device_output, device_error) == 0);
    CP_REQUIRE(device_error.str().empty());
    CP_REQUIRE(device_output.str().find("fake:loopback") != std::string::npos);
    CP_REQUIRE(device_output.str().find("Fake Loopback") != std::string::npos);

    std::ostringstream default_output;
    std::ostringstream default_error;
    CP_REQUIRE(cli::run_cli({}, dependencies, default_output, default_error) == 0);
    CP_REQUIRE(default_output.str().find("fake:loopback") != std::string::npos);

    std::ostringstream channel_output;
    std::ostringstream channel_error;
    CP_REQUIRE(cli::run_cli(
        {"channels", "--driver", "fake:loopback"},
        dependencies,
        channel_output,
        channel_error) == 0);
    CP_REQUIRE(channel_output.str().find("Input channels (8)") != std::string::npos);
    CP_REQUIRE(channel_output.str().find("[8] Output 8") != std::string::npos);
}

CP_TEST_CASE("CLI setup test runs end to end on the fake backend") {
    auto backend = std::make_shared<fake::FakeAudioBackend>();
    std::ostringstream output;
    std::ostringstream error;

    const auto exit_code = cli::run_cli(
        {
            "test",
            "--driver", "fake:loopback",
            "--play-channel", "1",
            "--record-channel", "1",
            "--verbose",
        },
        {.device_provider = backend, .capture_backend = backend},
        output,
        error);

    CP_REQUIRE(exit_code == 0);
    CP_REQUIRE(error.str().empty());
    CP_REQUIRE(output.str().find("Setup verification: passed") != std::string::npos);
    CP_REQUIRE(output.str().find("Direct score:") != std::string::npos);
    CP_REQUIRE(count_occurrences(output.str(), "Recording:") <= 22);
}

CP_TEST_CASE("CLI run handles UTF-8 paths and renders capture warnings") {
    TemporaryCliFiles files;
    const AudioBuffer source{
        .sample_rate = 1'000.0,
        .channel_count = 2,
        .samples = {
            0.10F, -0.10F,
            0.20F, -0.20F,
            0.30F, -0.30F,
            0.20F, -0.20F,
        },
    };
    write_wav(files.input, source, AudioBitDepth::pcm24);

    auto backend = std::make_shared<fake::FakeAudioBackend>();
    std::ostringstream output;
    std::ostringstream error;
    const auto exit_code = cli::run_cli(
        {
            "run",
            "--driver", "fake:loopback",
            "--play-channel", "1",
            "--record-channel", "1",
            "--input", path_utf8(files.input),
            "--output", path_utf8(files.output),
            "--verbose",
        },
        {.device_provider = backend, .capture_backend = backend},
        output,
        error);

    CP_REQUIRE(exit_code == 0);
    CP_REQUIRE(error.str().empty());
    CP_REQUIRE(std::filesystem::exists(files.output));
    CP_REQUIRE(output.str().find("Warning: The source has 2 channel(s)") != std::string::npos);
    CP_REQUIRE(output.str().find("Warning: \n") == std::string::npos);
    CP_REQUIRE(output.str().find(path_utf8(files.output)) != std::string::npos);
    const auto recorded = read_wav(files.output);
    CP_REQUIRE(recorded.audio.channel_count == 1);
    CP_REQUIRE(recorded.audio.frame_count() == source.frame_count());
}

CP_TEST_CASE("CLI help version license and usage errors do not require audio services") {
    const cli::Dependencies no_dependencies;
    std::ostringstream output;
    std::ostringstream error;

    CP_REQUIRE(cli::run_cli({"help", "run"}, no_dependencies, output, error) == 0);
    CP_REQUIRE(output.str().find("--bit-depth") != std::string::npos);

    output.str("");
    output.clear();
    CP_REQUIRE(cli::run_cli({"version"}, no_dependencies, output, error) == 0);
    CP_REQUIRE(output.str().find("capture-panel") != std::string::npos);

    output.str("");
    output.clear();
    CP_REQUIRE(cli::run_cli({"license"}, no_dependencies, output, error) == 0);
    CP_REQUIRE(output.str().find("GPL") != std::string::npos);
    CP_REQUIRE(!warning_message(CaptureWarning::marker_evidence_low).empty());
    CP_REQUIRE(!failure_message(CaptureFailure::digital_clipping).empty());

    output.str("");
    output.clear();
    CP_REQUIRE(cli::run_cli({"run", "--bit-depth", "20"}, no_dependencies, output, error) == 2);
    CP_REQUIRE(error.str().find("16, 24, or 32") != std::string::npos);
}
