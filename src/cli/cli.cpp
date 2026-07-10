#include "cli.hpp"

#include "capture_panel/core/capture.hpp"
#include "capture_panel/core/channels.hpp"
#include "capture_panel/core/diagnostics.hpp"
#include "capture_panel/core/errors.hpp"

#include <cmath>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#ifndef CAPTURE_PANEL_VERSION
#define CAPTURE_PANEL_VERSION "0.1.0"
#endif

namespace capture_panel::cli {
namespace {

constexpr int success_exit_code = 0;
constexpr int runtime_error_exit_code = 1;
constexpr int usage_error_exit_code = 2;

[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view value) {
    const auto* first = reinterpret_cast<const char8_t*>(value.data());
    return std::filesystem::path(std::u8string(first, first + value.size()));
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

[[nodiscard]] std::vector<std::uint32_t> parse_channels(const std::string& specification) {
    try {
        return parse_channel_spec(specification);
    } catch (const CaptureError& error) {
        throw std::invalid_argument(error.what());
    }
}

[[nodiscard]] double parse_finite_double(const std::string& value, std::string_view label) {
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stod(value, &consumed);
        if (consumed != value.size() || !std::isfinite(parsed)) {
            throw std::invalid_argument("not finite");
        }
        return parsed;
    } catch (...) {
        throw std::invalid_argument(std::string(label) + " must be a finite number: " + value);
    }
}

[[nodiscard]] AudioBitDepth parse_bit_depth(const std::string& value) {
    if (value == "16") return AudioBitDepth::pcm16;
    if (value == "24") return AudioBitDepth::pcm24;
    if (value == "32") return AudioBitDepth::pcm32;
    throw std::invalid_argument("--bit-depth must be 16, 24, or 32.");
}

[[nodiscard]] bool matches_option(const std::string& argument, std::string_view name) {
    return std::string_view(argument) == name || argument.starts_with(std::string(name) + '=');
}

[[nodiscard]] std::string option_value(
    const std::vector<std::string>& args,
    std::size_t& index,
    std::string_view long_name,
    std::optional<std::string_view> short_name = std::nullopt) {
    const auto& argument = args[index];
    const auto equals = argument.find('=');
    if (equals != std::string::npos) {
        const auto value = argument.substr(equals + 1);
        if (value.empty()) throw std::invalid_argument(std::string(long_name) + " requires a value.");
        return value;
    }

    const bool matches_long = std::string_view(argument) == long_name;
    const bool matches_short = short_name && std::string_view(argument) == *short_name;
    if (!matches_long && !matches_short) {
        throw std::logic_error("option_value called for a different option");
    }
    if (++index >= args.size()) {
        throw std::invalid_argument(std::string(long_name) + " requires a value.");
    }
    return args[index];
}

[[nodiscard]] Command parse_command_name(const std::string& name) {
    if (name == "devices") return Command::devices;
    if (name == "channels") return Command::channels;
    if (name == "test") return Command::test;
    if (name == "run") return Command::run;
    if (name == "help" || name == "--help" || name == "-h") return Command::help;
    if (name == "version" || name == "--version") return Command::version;
    if (name == "license" || name == "--license") return Command::license;
    throw std::invalid_argument("Unknown command: " + name);
}

void require_option(bool present, std::string_view option, std::string_view command) {
    if (!present) {
        throw std::invalid_argument(
            std::string(command) + " requires " + std::string(option) + '.');
    }
}

[[nodiscard]] const char* direction_label(ChannelDirection direction) {
    return direction == ChannelDirection::input ? "Input" : "Output";
}

void print_general_help(std::ostream& output) {
    output
        << "Capture Panel " CAPTURE_PANEL_VERSION "\n"
        << "Play a WAV file through external audio hardware and capture its return.\n\n"
        << "Usage: capture-panel <command> [options]\n\n"
        << "Commands:\n"
        << "  devices   List available audio drivers\n"
        << "  channels  List a driver's input and output channels\n"
        << "  test      Verify a playback/recording route\n"
        << "  run       Capture a WAV file\n"
        << "  help      Show general or command-specific help\n"
        << "  version   Print the version\n"
        << "  license   Print license information\n\n"
        << "Run 'capture-panel help <command>' for command options.\n";
}

void print_command_help(const std::string& topic, std::ostream& output) {
    if (topic.empty()) {
        print_general_help(output);
    } else if (topic == "devices") {
        output << "Usage: capture-panel devices [--inputs|-i | --outputs|-o]\n";
    } else if (topic == "channels") {
        output << "Usage: capture-panel channels --driver <id>\n";
    } else if (topic == "test") {
        output
            << "Usage: capture-panel test --driver <id> --play-channel <spec> "
               "--record-channel <spec> [--output-trim <dB>] [--verbose]\n";
    } else if (topic == "run") {
        output
            << "Usage: capture-panel run --input <source.wav> --output <result.wav> "
               "--driver <id> --play-channel <spec> --record-channel <spec> "
               "[--bit-depth <16|24|32>] [--output-trim <dB>] [--verbose]\n";
    } else if (topic == "help" || topic == "version" || topic == "license") {
        output << "Usage: capture-panel " << topic << "\n";
    } else {
        throw std::invalid_argument("Unknown help topic: " + topic);
    }
}

void require_dependencies(const Dependencies& dependencies) {
    if (!dependencies.device_provider || !dependencies.capture_backend) {
        throw std::runtime_error("CLI audio services are not configured.");
    }
}

[[nodiscard]] std::string join_channels(const std::vector<std::uint32_t>& channels) {
    std::ostringstream result;
    for (std::size_t index = 0; index < channels.size(); ++index) {
        if (index != 0) result << ',';
        result << channels[index];
    }
    return result.str();
}

void print_devices(
    const ParsedCommand& command,
    const Dependencies& dependencies,
    std::ostream& output) {
    if (!dependencies.device_provider) throw std::runtime_error("Audio device provider is not configured.");

    std::vector<AudioDevice> visible_devices;
    for (const auto& device : dependencies.device_provider->devices()) {
        if (command.inputs_only && !device.has_input()) continue;
        if (command.outputs_only && !device.has_output()) continue;
        visible_devices.push_back(device);
    }
    if (visible_devices.empty()) {
        output << "No audio drivers found.\n";
        return;
    }

    output << "ID\tName\tInputs\tOutputs\tSample rate\tStatus\n";
    for (const auto& device : visible_devices) {
        output << device.id << '\t' << device.name << '\t'
               << device.input_channels << '\t' << device.output_channels << '\t'
               << static_cast<std::int64_t>(std::llround(device.sample_rate)) << " Hz\t"
               << (device.available ? "available" : "unavailable");
        if (!device.status.empty()) output << " (" << device.status << ')';
        output << '\n';
    }
}

void print_channels(
    const ParsedCommand& command,
    const Dependencies& dependencies,
    std::ostream& output) {
    if (!dependencies.device_provider) throw std::runtime_error("Audio device provider is not configured.");

    const auto device = dependencies.device_provider->device(command.driver_id);
    output << device.name << " (" << device.id << ")\n"
           << "Sample rate: " << static_cast<std::int64_t>(std::llround(device.sample_rate)) << " Hz\n";
    for (const auto direction : {ChannelDirection::input, ChannelDirection::output}) {
        const auto channels = dependencies.device_provider->channels(command.driver_id, direction);
        output << direction_label(direction) << " channels (" << channels.size() << "):\n";
        for (const auto& channel : channels) {
            output << "  [" << channel.index << "] " << channel.name << '\n';
        }
    }
}

[[nodiscard]] const char* stage_label(CaptureStage stage) {
    switch (stage) {
    case CaptureStage::sample_rate_configuration: return "sample-rate configuration";
    case CaptureStage::recording: return "recording";
    case CaptureStage::alignment: return "alignment";
    case CaptureStage::verification: return "verification";
    case CaptureStage::output_writing: return "output writing";
    }
    return "unknown";
}

[[nodiscard]] CaptureEventHandler event_reporter(bool verbose, std::ostream& output) {
    return [verbose, &output, last_percentage = -1](const CaptureEvent& event) mutable {
        if (event.type == CaptureEventType::stage_changed && event.stage) {
            output << "Stage: " << stage_label(*event.stage) << '\n';
            if (verbose && event.sample_rate) {
                output << "  Sample rate: "
                       << static_cast<std::int64_t>(std::llround(*event.sample_rate))
                       << " Hz\n";
            }
            if (verbose && event.total_frames) {
                output << "  Playback frames: " << *event.total_frames << '\n';
            }
            if (verbose && event.padding_seconds) {
                output << "  Pre/post padding: " << *event.padding_seconds << " seconds\n";
            }
            if (verbose && event.marker_to_payload_silence_seconds) {
                output << "  Marker wait: " << *event.marker_to_payload_silence_seconds
                       << " seconds\n";
            }
        } else if (event.type == CaptureEventType::recording_progress && event.progress) {
            const auto percentage = event.progress->percentage();
            const auto reporting_interval = verbose ? 5 : 25;
            if (last_percentage < 0 || percentage == 100
                || percentage / reporting_interval
                    > last_percentage / reporting_interval) {
                output << "Recording: " << percentage << '%';
                if (verbose && event.progress->remaining_seconds() > 0.0) {
                    output << " (" << std::fixed << std::setprecision(1)
                           << event.progress->remaining_seconds() << " seconds remaining)";
                }
                output << '\n';
                last_percentage = percentage;
            }
        } else if (event.type == CaptureEventType::warning) {
            output << "Warning: ";
            if (!event.message.empty()) {
                output << event.message;
            } else if (event.warning) {
                output << warning_message(*event.warning);
            } else {
                output << "Unspecified capture warning.";
            }
            output << '\n';
        } else if (verbose && event.type == CaptureEventType::input_loaded && event.input) {
            output << "Input: " << path_utf8(event.input->path) << '\n'
                   << "  Sample rate: "
                   << static_cast<std::int64_t>(std::llround(event.input->format.sample_rate))
                   << " Hz\n"
                   << "  Channels: " << event.input->format.channel_count << '\n'
                   << "  Bit depth: " << static_cast<unsigned>(event.input->format.bit_depth) << '\n'
                   << "  Frames: " << event.input->format.total_frames << '\n';
        } else if (verbose && event.type == CaptureEventType::impulse_detection
                   && event.impulse_detection) {
            output << "Expected markers: "
                   << event.impulse_detection->expected_positions.size() << '\n'
                   << "Detected markers: "
                   << event.impulse_detection->detected_positions.size() << '\n';
        } else if (verbose && event.type == CaptureEventType::alignment_finished
                   && event.alignment) {
            if (event.alignment->marker_latency_samples) {
                output << "Marker latency: "
                       << *event.alignment->marker_latency_samples << " frames\n";
            }
            output << "Trim start: " << event.alignment->trim_start_frame << " frames\n";
        } else if (verbose && !event.message.empty()) {
            output << event.message << '\n';
        }
    };
}

int execute_capture(
    const ParsedCommand& command,
    const Dependencies& dependencies,
    std::ostream& output) {
    require_dependencies(dependencies);
    CaptureService service(
        dependencies.device_provider,
        dependencies.capture_backend,
        event_reporter(command.verbose, output));

    CaptureConfiguration configuration{
        .input_path = command.input_path,
        .output_path = command.output_path,
        .route = {
            .driver_id = command.driver_id,
            .playback_channels = command.playback_channels,
            .record_channels = command.record_channels,
        },
        .output_bit_depth = command.bit_depth,
    };
    const CapturePassOptions options{.playback_gain_db = command.output_trim_db};
    const auto result = service.capture(configuration, options);

    output << "Capture complete\n"
           << "Output: " << path_utf8(result.output.path) << '\n'
           << "Size: " << result.output.file_size << " bytes\n"
           << "Channels: " << result.output.channel_count << '\n'
           << "Bit depth: " << static_cast<unsigned>(result.output.bit_depth) << '\n'
           << "Sample rate: " << static_cast<std::int64_t>(std::llround(result.output.sample_rate)) << " Hz\n"
           << "Elapsed: " << std::fixed << std::setprecision(2) << result.elapsed.count() << " seconds\n";
    if (result.alignment.marker_latency_samples) {
        output << "Latency: " << *result.alignment.marker_latency_samples << " frames";
        if (result.alignment.marker_latency_milliseconds) {
            output << " (" << *result.alignment.marker_latency_milliseconds << " ms)";
        }
        output << '\n';
    }
    output << "Trim start: " << result.alignment.trim_start_frame << " frames\n"
           << "Trimmed frames: " << result.alignment.trimmed_frame_count
           << " / " << result.alignment.target_frame_count << '\n';
    return success_exit_code;
}

int execute_test(
    const ParsedCommand& command,
    const Dependencies& dependencies,
    std::ostream& output) {
    require_dependencies(dependencies);
    CaptureService service(
        dependencies.device_provider,
        dependencies.capture_backend,
        event_reporter(command.verbose, output));
    const CaptureRoute route{
        .driver_id = command.driver_id,
        .playback_channels = command.playback_channels,
        .record_channels = command.record_channels,
    };

    output << "Testing route\n"
           << "Driver: " << command.driver_id << '\n'
           << "Playback channels: " << join_channels(command.playback_channels) << '\n'
           << "Recording channels: " << join_channels(command.record_channels) << '\n';
    const auto result = service.verify_setup(
        route,
        std::nullopt,
        command.output_trim_db,
        0.0);

    output << "Setup verification: " << (result.passed() ? "passed" : "failed") << '\n'
           << "Sample rate: " << static_cast<std::int64_t>(std::llround(result.sample_rate)) << " Hz\n"
           << "Output peak: " << std::fixed << std::setprecision(2)
           << result.output_peak_dbfs << " dBFS\n"
           << "Input peak: " << result.input_peak_dbfs << " dBFS\n";
    if (result.alignment.marker_latency_samples) {
        output << "Latency: " << *result.alignment.marker_latency_samples << " frames";
        if (result.alignment.marker_latency_milliseconds) {
            output << " (" << *result.alignment.marker_latency_milliseconds << " ms)";
        }
        output << '\n';
    }
    if (result.verification.timing_fit_error_frames) {
        const auto milliseconds = *result.verification.timing_fit_error_frames
            / result.sample_rate * 1'000.0;
        output << "Timing error: " << *result.verification.timing_fit_error_frames
               << " frames (" << std::setprecision(4) << milliseconds << " ms)\n";
    } else {
        output << "Timing verification: not available\n";
    }

    output << "Warnings: " << result.warnings.size() << '\n';
    for (const auto warning : result.warnings) {
        output << "  - " << warning_message(warning) << '\n';
    }
    output << "Failures: " << result.failures.size() << '\n';
    for (const auto failure : result.failures) {
        output << "  - " << failure_message(failure) << '\n';
    }

    if (command.verbose) {
        output << "Trim start: " << result.alignment.trim_start_frame << " frames\n"
               << "Trimmed frames: " << result.alignment.trimmed_frame_count
               << " / " << result.alignment.target_frame_count << '\n';
        if (result.verification.start_offset_frames) {
            output << "Start offset: " << *result.verification.start_offset_frames << " frames\n";
        }
        if (result.verification.max_timing_error_frames) {
            output << "Maximum timing error: "
                   << *result.verification.max_timing_error_frames << " frames\n";
        }
        if (result.verification.sweep) {
            const auto& sweep = *result.verification.sweep;
            output << "Direct score: " << std::setprecision(4) << sweep.direct_score << '\n';
            if (sweep.detected_frame) {
                output << "Detected sweep frame: " << *sweep.detected_frame << '\n';
            }
            if (sweep.strongest_frame) {
                output << "Strongest match: frame " << *sweep.strongest_frame;
                if (sweep.strongest_offset_frames) {
                    output << ", offset " << std::showpos << *sweep.strongest_offset_frames
                           << std::noshowpos << " frames";
                }
                output << ", score " << sweep.strongest_score << '\n';
            }
            if (sweep.ambiguity_ratio) {
                output << "Ambiguity ratio: " << *sweep.ambiguity_ratio << '\n';
            }
            output << "Reliability: " << reliability_name(sweep.reliability) << '\n';
        }
        if (result.verification.ambiguous_match_count > 0) {
            output << "Ambiguous matches: "
                   << result.verification.ambiguous_match_count << '\n';
        }
    }
    return result.passed() ? success_exit_code : runtime_error_exit_code;
}

int execute(
    const ParsedCommand& command,
    const Dependencies& dependencies,
    std::ostream& output) {
    switch (command.command) {
    case Command::help:
        print_command_help(command.help_topic, output);
        return success_exit_code;
    case Command::version:
        output << "capture-panel " CAPTURE_PANEL_VERSION "\n";
        return success_exit_code;
    case Command::license:
        output
            << "Capture Panel for Windows is licensed under GNU GPL version 3 only.\n"
            << "This program comes with ABSOLUTELY NO WARRANTY. You may redistribute\n"
            << "and modify it under GPLv3. See the bundled LICENSE file for the full terms.\n";
        return success_exit_code;
    case Command::devices:
        print_devices(command, dependencies, output);
        return success_exit_code;
    case Command::channels:
        print_channels(command, dependencies, output);
        return success_exit_code;
    case Command::test:
        return execute_test(command, dependencies, output);
    case Command::run:
        return execute_capture(command, dependencies, output);
    }
    throw std::logic_error("Unhandled CLI command.");
}

} // namespace

ParsedCommand parse_arguments(const std::vector<std::string>& args) {
    ParsedCommand parsed;
    bool output_trim_seen = false;
    if (args.empty()) return parsed;

    parsed.command = parse_command_name(args.front());
    if (parsed.command == Command::help) {
        if (args.size() > 2) throw std::invalid_argument("help accepts at most one command name.");
        if (args.size() == 2) parsed.help_topic = args[1];
        return parsed;
    }
    if (parsed.command == Command::version || parsed.command == Command::license) {
        if (args.size() != 1) throw std::invalid_argument(args.front() + " does not accept options.");
        return parsed;
    }

    for (std::size_t index = 1; index < args.size(); ++index) {
        const auto& argument = args[index];
        if (argument == "--help" || argument == "-h") {
            parsed.help_topic = args.front();
            parsed.command = Command::help;
            if (index + 1 != args.size()) throw std::invalid_argument("--help must be the final argument.");
            return parsed;
        }
        if (argument == "--verbose" || argument == "-v") {
            parsed.verbose = true;
        } else if (argument == "--inputs"
                   || (parsed.command == Command::devices && argument == "-i")) {
            parsed.inputs_only = true;
        } else if (argument == "--outputs"
                   || (parsed.command == Command::devices && argument == "-o")) {
            parsed.outputs_only = true;
        } else if (matches_option(argument, "--driver")) {
            parsed.driver_id = option_value(args, index, "--driver");
        } else if (matches_option(argument, "--play-channel")) {
            parsed.playback_channels = parse_channels(option_value(args, index, "--play-channel"));
        } else if (matches_option(argument, "--record-channel")) {
            parsed.record_channels = parse_channels(option_value(args, index, "--record-channel"));
        } else if (matches_option(argument, "--input") || argument == "-i") {
            parsed.input_path = path_from_utf8(option_value(args, index, "--input", "-i"));
        } else if (matches_option(argument, "--output") || argument == "-o") {
            parsed.output_path = path_from_utf8(option_value(args, index, "--output", "-o"));
        } else if (matches_option(argument, "--bit-depth")) {
            parsed.bit_depth = parse_bit_depth(option_value(args, index, "--bit-depth"));
        } else if (matches_option(argument, "--output-trim")) {
            output_trim_seen = true;
            parsed.output_trim_db = parse_finite_double(
                option_value(args, index, "--output-trim"),
                "--output-trim");
        } else if (parsed.command == Command::channels && !argument.empty()
                   && argument.front() != '-' && parsed.driver_id.empty()) {
            // Positional driver IDs are accepted for parity with the macOS CLI.
            parsed.driver_id = argument;
        } else {
            throw std::invalid_argument("Unknown option: " + argument);
        }
    }

    if (parsed.inputs_only && parsed.outputs_only) {
        throw std::invalid_argument("--inputs and --outputs cannot be used together.");
    }
    if (parsed.command != Command::devices && (parsed.inputs_only || parsed.outputs_only)) {
        throw std::invalid_argument("--inputs and --outputs are only valid for devices.");
    }
    if (parsed.command == Command::devices
        && (!parsed.driver_id.empty() || !parsed.playback_channels.empty()
            || !parsed.record_channels.empty() || !parsed.input_path.empty()
            || !parsed.output_path.empty() || parsed.bit_depth || output_trim_seen
            || parsed.verbose)) {
        throw std::invalid_argument("devices only accepts --inputs or --outputs.");
    }
    if (parsed.command == Command::channels) {
        require_option(!parsed.driver_id.empty(), "--driver", "channels");
        if (!parsed.playback_channels.empty() || !parsed.record_channels.empty()
            || !parsed.input_path.empty() || !parsed.output_path.empty()
            || parsed.bit_depth || output_trim_seen || parsed.verbose) {
            throw std::invalid_argument("channels only accepts a driver ID.");
        }
    }
    if (parsed.command == Command::test || parsed.command == Command::run) {
        const auto command_name = parsed.command == Command::test ? "test" : "run";
        require_option(!parsed.driver_id.empty(), "--driver", command_name);
        require_option(!parsed.playback_channels.empty(), "--play-channel", command_name);
        require_option(!parsed.record_channels.empty(), "--record-channel", command_name);
    }
    if (parsed.command == Command::run) {
        require_option(!parsed.input_path.empty(), "--input", "run");
        require_option(!parsed.output_path.empty(), "--output", "run");
    }
    if (parsed.command != Command::run &&
        (!parsed.input_path.empty() || !parsed.output_path.empty() || parsed.bit_depth)) {
        throw std::invalid_argument("--input, --output, and --bit-depth are only valid for run.");
    }

    return parsed;
}

int run_cli(
    const std::vector<std::string>& args,
    Dependencies dependencies,
    std::ostream& output,
    std::ostream& error) {
    try {
        return execute(parse_arguments(args), dependencies, output);
    } catch (const std::invalid_argument& exception) {
        error << "Error: " << exception.what() << "\nTry 'capture-panel help'.\n";
        return usage_error_exit_code;
    } catch (const CaptureError& exception) {
        error << "Capture error: " << exception.what() << '\n';
        return runtime_error_exit_code;
    } catch (const std::exception& exception) {
        error << "Error: " << exception.what() << '\n';
        return runtime_error_exit_code;
    }
}

int run_cli(
    int argc,
    char* argv[],
    Dependencies dependencies,
    std::ostream& output,
    std::ostream& error) {
    std::vector<std::string> args;
    args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : std::size_t{0});
    for (int index = 1; index < argc; ++index) args.emplace_back(argv[index]);
    return run_cli(args, std::move(dependencies), output, error);
}

} // namespace capture_panel::cli
