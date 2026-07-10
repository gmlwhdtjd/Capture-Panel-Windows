#pragma once

#include "capture_panel/core/backend.hpp"
#include "capture_panel/core/types.hpp"

#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
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
    bool verbose = false;
    std::string help_topic;
};

struct Dependencies {
    std::shared_ptr<IAudioDeviceProvider> device_provider;
    std::shared_ptr<IAudioCaptureBackend> capture_backend;
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

} // namespace capture_panel::cli
