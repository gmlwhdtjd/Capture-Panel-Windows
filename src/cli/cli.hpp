#pragma once

#include "capture_panel/core/backend.hpp"
#include "capture_panel/core/types.hpp"

#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace capture_panel::cli {

enum class Command {
    devices,
    channels,
    test,
    run,
    help,
    version,
    license,
};

struct ParsedCommand {
    Command command = Command::devices;
    bool inputs_only = false;
    bool outputs_only = false;
    std::string driver_id;
    std::vector<std::uint32_t> playback_channels;
    std::vector<std::uint32_t> record_channels;
    std::filesystem::path input_path;
    std::filesystem::path output_path;
    std::optional<AudioBitDepth> bit_depth;
    double output_trim_db = 0.0;
    double input_trim_db = 0.0;
    std::optional<double> sample_rate;
    bool json = false;
    bool verbose = false;
    std::string help_topic;
};

struct Dependencies {
    std::shared_ptr<IAudioDeviceProvider> device_provider;
    std::shared_ptr<IAudioCaptureBackend> capture_backend;
    std::shared_ptr<CancellationToken> cancellation;
};

// args excludes argv[0]. Throws std::invalid_argument for usage errors.
[[nodiscard]] ParsedCommand parse_arguments(const std::vector<std::string>& args);

int run_cli(
    const std::vector<std::string>& args,
    Dependencies dependencies,
    std::ostream& output,
    std::ostream& error);

int run_cli(
    int argc,
    char* argv[],
    Dependencies dependencies,
    std::ostream& output,
    std::ostream& error);

// Used by the executable for failures that happen before run_cli can take
// ownership of the command. Keeps JSON-mode startup failures on the same wire
// protocol as command failures.
int report_startup_error(
    std::string_view message,
    bool json_requested,
    std::ostream& output,
    std::ostream& error);

} // namespace capture_panel::cli
