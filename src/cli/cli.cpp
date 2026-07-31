#include "cli.hpp"

#include "capture_panel/core/capture.hpp"
#include "capture_panel/core/channels.hpp"
#include "capture_panel/core/constants.hpp"
#include "capture_panel/core/diagnostics.hpp"
#include "capture_panel/core/errors.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#ifndef CAPTURE_PANEL_VERSION
#define CAPTURE_PANEL_VERSION "0.2.3"
#endif

namespace capture_panel::cli {
namespace {

constexpr int success_exit_code = 0;
constexpr int runtime_error_exit_code = 1;
constexpr int usage_error_exit_code = 2;
constexpr std::string_view json_protocol = "capture-panel/1";

[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view value) {
    const auto* first = reinterpret_cast<const char8_t*>(value.data());
    return std::filesystem::path(std::u8string(first, first + value.size()));
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

[[nodiscard]] bool is_utf8_continuation(const unsigned char value) noexcept {
    return value >= 0x80U && value <= 0xBFU;
}

[[nodiscard]] std::size_t valid_utf8_sequence_length(
    const std::string_view value,
    const std::size_t offset) noexcept {
    const auto remaining = value.size() - offset;
    const auto first = static_cast<unsigned char>(value[offset]);
    if (first <= 0x7FU) return 1;
    if (first >= 0xC2U && first <= 0xDFU) {
        return remaining >= 2
                && is_utf8_continuation(static_cast<unsigned char>(value[offset + 1]))
            ? 2U
            : 0U;
    }
    if (remaining >= 3) {
        const auto second = static_cast<unsigned char>(value[offset + 1]);
        const auto third = static_cast<unsigned char>(value[offset + 2]);
        const auto valid_tail = is_utf8_continuation(third);
        if (first == 0xE0U && second >= 0xA0U && second <= 0xBFU && valid_tail) return 3;
        if (first >= 0xE1U && first <= 0xECU
            && is_utf8_continuation(second) && valid_tail) return 3;
        if (first == 0xEDU && second >= 0x80U && second <= 0x9FU && valid_tail) return 3;
        if (first >= 0xEEU && first <= 0xEFU
            && is_utf8_continuation(second) && valid_tail) return 3;
    }
    if (remaining >= 4) {
        const auto second = static_cast<unsigned char>(value[offset + 1]);
        const auto third = static_cast<unsigned char>(value[offset + 2]);
        const auto fourth = static_cast<unsigned char>(value[offset + 3]);
        const auto valid_tail = is_utf8_continuation(third) && is_utf8_continuation(fourth);
        if (first == 0xF0U && second >= 0x90U && second <= 0xBFU && valid_tail) return 4;
        if (first >= 0xF1U && first <= 0xF3U
            && is_utf8_continuation(second) && valid_tail) return 4;
        if (first == 0xF4U && second >= 0x80U && second <= 0x8FU && valid_tail) return 4;
    }
    return 0;
}

void append_json_string(std::string& destination, const std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    destination.push_back('"');
    for (std::size_t offset = 0; offset < value.size();) {
        const auto byte = static_cast<unsigned char>(value[offset]);
        if (byte >= 0x80U) {
            const auto length = valid_utf8_sequence_length(value, offset);
            if (length == 0) {
                destination += "\\uFFFD";
                ++offset;
            } else {
                destination.append(value.substr(offset, length));
                offset += length;
            }
            continue;
        }

        switch (byte) {
        case '"': destination += "\\\""; break;
        case '\\': destination += "\\\\"; break;
        case '\b': destination += "\\b"; break;
        case '\f': destination += "\\f"; break;
        case '\n': destination += "\\n"; break;
        case '\r': destination += "\\r"; break;
        case '\t': destination += "\\t"; break;
        default:
            if (byte < 0x20U) {
                destination += "\\u00";
                destination.push_back(hex[(byte >> 4U) & 0x0FU]);
                destination.push_back(hex[byte & 0x0FU]);
            } else {
                destination.push_back(static_cast<char>(byte));
            }
            break;
        }
        ++offset;
    }
    destination.push_back('"');
}

template <typename Integer>
void append_json_integer(std::string& destination, const Integer value) {
    std::array<char, 32> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (error != std::errc{}) {
        destination += "null";
        return;
    }
    destination.append(buffer.data(), end);
}

void append_json_number(std::string& destination, const double value) {
    if (!std::isfinite(value)) {
        destination += "null";
        return;
    }
    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(
        buffer.data(),
        buffer.data() + buffer.size(),
        value,
        std::chars_format::general);
    if (error != std::errc{}) {
        destination += "null";
        return;
    }
    destination.append(buffer.data(), end);
}

[[nodiscard]] std::string invariant_number_text(const double value) {
    std::string result;
    append_json_number(result, value);
    return result;
}

class JsonObject final {
public:
    JsonObject() { value_.push_back('{'); }

    void string_field(const std::string_view name, const std::string_view value) {
        field_prefix(name);
        append_json_string(value_, value);
    }

    template <typename Integer>
    void integer_field(const std::string_view name, const Integer value) {
        field_prefix(name);
        append_json_integer(value_, value);
    }

    void number_field(const std::string_view name, const double value) {
        field_prefix(name);
        append_json_number(value_, value);
    }

    void boolean_field(const std::string_view name, const bool value) {
        field_prefix(name);
        value_ += value ? "true" : "false";
    }

    void null_field(const std::string_view name) {
        field_prefix(name);
        value_ += "null";
    }

    void raw_field(const std::string_view name, const std::string_view json) {
        field_prefix(name);
        value_.append(json);
    }

    [[nodiscard]] std::string finish() {
        value_.push_back('}');
        return std::move(value_);
    }

private:
    void field_prefix(const std::string_view name) {
        if (!first_) value_.push_back(',');
        first_ = false;
        append_json_string(value_, name);
        value_.push_back(':');
    }

    std::string value_;
    bool first_ = true;
};

template <typename Range, typename Mapper>
[[nodiscard]] std::string json_array(const Range& values, Mapper mapper) {
    std::string result{"["};
    auto first = true;
    for (const auto& value : values) {
        if (!first) result.push_back(',');
        first = false;
        result += mapper(value);
    }
    result.push_back(']');
    return result;
}

[[nodiscard]] std::string json_integer(const auto value) {
    std::string result;
    append_json_integer(result, value);
    return result;
}

[[nodiscard]] std::string json_device(const AudioDevice& device) {
    JsonObject object;
    object.string_field("id", device.id);
    object.string_field("name", device.name);
    object.integer_field("inputChannels", device.input_channels);
    object.integer_field("outputChannels", device.output_channels);
    object.number_field("sampleRate", device.sample_rate);
    object.boolean_field("available", device.available);
    object.string_field("status", device.status);
    return object.finish();
}

[[nodiscard]] std::string json_channel(const AudioChannel& channel) {
    JsonObject object;
    object.integer_field("index", channel.index);
    object.string_field("name", channel.name);
    return object.finish();
}

[[nodiscard]] std::string json_route(const CaptureRoute& route) {
    JsonObject object;
    object.string_field("driverId", route.driver_id);
    object.raw_field("playbackChannels", json_array(
        route.playback_channels,
        [](const std::uint32_t channel) { return json_integer(channel); }));
    object.raw_field("recordChannels", json_array(
        route.record_channels,
        [](const std::uint32_t channel) { return json_integer(channel); }));
    return object.finish();
}

[[nodiscard]] std::string json_wav_format(const WavFormat& format) {
    JsonObject object;
    object.number_field("sampleRate", format.sample_rate);
    object.integer_field("channelCount", format.channel_count);
    object.integer_field("bitDepth", static_cast<std::uint16_t>(format.bit_depth));
    object.integer_field("totalFrames", format.total_frames);
    return object.finish();
}

[[nodiscard]] std::string json_input_info(const CaptureInputInfo& input) {
    JsonObject object;
    object.string_field("path", path_utf8(input.path));
    object.raw_field("format", json_wav_format(input.format));
    return object.finish();
}

[[nodiscard]] std::string json_output_info(const CaptureOutputInfo& output) {
    JsonObject object;
    object.string_field("path", path_utf8(output.path));
    object.integer_field("fileSize", output.file_size);
    object.number_field("sampleRate", output.sample_rate);
    object.integer_field("channelCount", output.channel_count);
    object.integer_field("bitDepth", static_cast<std::uint16_t>(output.bit_depth));
    return object.finish();
}

[[nodiscard]] std::string json_alignment(const PayloadAlignmentInfo& alignment) {
    JsonObject object;
    if (alignment.marker_latency_samples) {
        object.integer_field("markerLatencyFrames", *alignment.marker_latency_samples);
    } else {
        object.null_field("markerLatencyFrames");
    }
    if (alignment.marker_latency_milliseconds) {
        object.number_field("markerLatencyMilliseconds", *alignment.marker_latency_milliseconds);
    } else {
        object.null_field("markerLatencyMilliseconds");
    }
    object.integer_field("trimStartFrame", alignment.trim_start_frame);
    object.integer_field("trimmedFrameCount", alignment.trimmed_frame_count);
    object.integer_field("targetFrameCount", alignment.target_frame_count);
    return object.finish();
}

[[nodiscard]] std::string json_impulse_detection(const ImpulseDetection& impulse) {
    JsonObject object;
    object.raw_field("expectedPositions", json_array(
        impulse.expected_positions,
        [](const std::int64_t frame) { return json_integer(frame); }));
    object.raw_field("detectedPositions", json_array(
        impulse.detected_positions,
        [](const std::int64_t frame) { return json_integer(frame); }));
    return object.finish();
}

[[nodiscard]] std::string json_warning(const CaptureWarning warning) {
    JsonObject object;
    object.string_field("code", warning_name(warning));
    object.string_field("message", warning_message(warning));
    return object.finish();
}

[[nodiscard]] std::string json_failure(const CaptureFailure failure) {
    JsonObject object;
    object.string_field("code", failure_name(failure));
    object.string_field("message", failure_message(failure));
    return object.finish();
}

[[nodiscard]] std::string json_sweep(const VerificationSweepResult& sweep) {
    JsonObject object;
    object.integer_field("expectedFrame", sweep.expected_frame);
    if (sweep.detected_frame) object.integer_field("detectedFrame", *sweep.detected_frame);
    else object.null_field("detectedFrame");
    if (sweep.error_frames) object.integer_field("errorFrames", *sweep.error_frames);
    else object.null_field("errorFrames");
    object.number_field("directScore", sweep.direct_score);
    if (sweep.strongest_frame) object.integer_field("strongestFrame", *sweep.strongest_frame);
    else object.null_field("strongestFrame");
    if (sweep.strongest_offset_frames) {
        object.integer_field("strongestOffsetFrames", *sweep.strongest_offset_frames);
    } else {
        object.null_field("strongestOffsetFrames");
    }
    object.number_field("strongestScore", sweep.strongest_score);
    if (sweep.ambiguity_ratio) object.number_field("ambiguityRatio", *sweep.ambiguity_ratio);
    else object.null_field("ambiguityRatio");
    object.string_field("reliability", reliability_name(sweep.reliability));
    return object.finish();
}

[[nodiscard]] std::string json_verification(const AlignmentVerificationResult& verification) {
    JsonObject object;
    if (verification.start_offset_frames) {
        object.integer_field("startOffsetFrames", *verification.start_offset_frames);
    } else {
        object.null_field("startOffsetFrames");
    }
    if (verification.timing_fit_error_frames) {
        object.number_field("timingFitErrorFrames", *verification.timing_fit_error_frames);
    } else {
        object.null_field("timingFitErrorFrames");
    }
    if (verification.max_timing_error_frames) {
        object.integer_field("maxTimingErrorFrames", *verification.max_timing_error_frames);
    } else {
        object.null_field("maxTimingErrorFrames");
    }
    if (verification.sweep) object.raw_field("sweep", json_sweep(*verification.sweep));
    else object.null_field("sweep");
    object.integer_field("ambiguousMatchCount", verification.ambiguous_match_count);
    object.raw_field("warnings", json_array(
        verification.warnings,
        [](const CaptureWarning warning) { return json_warning(warning); }));
    object.raw_field("failures", json_array(
        verification.failures,
        [](const CaptureFailure failure) { return json_failure(failure); }));
    return object.finish();
}

[[nodiscard]] std::string json_progress(const CaptureProgress& progress) {
    JsonObject object;
    object.integer_field("completedFrames", progress.completed_frames);
    object.integer_field("totalFrames", progress.total_frames);
    object.number_field("sampleRate", progress.sample_rate);
    object.integer_field("percentage", progress.percentage());
    object.number_field("remainingSeconds", progress.remaining_seconds());
    return object.finish();
}

void write_json_line(std::ostream& output, const std::string_view json) {
    output << json << '\n';
    output.flush();
}

[[nodiscard]] JsonObject protocol_object(const std::string_view type) {
    JsonObject object;
    object.string_field("protocol", json_protocol);
    object.string_field("type", type);
    return object;
}

[[nodiscard]] std::string json_capture_event(const CaptureEvent& event) {
    auto object = protocol_object("event");
    object.string_field("event", event_type_name(event.type));
    if (event.stage) object.string_field("stage", stage_name(*event.stage));
    if (event.progress) object.raw_field("progress", json_progress(*event.progress));
    if (event.warning) object.raw_field("warning", json_warning(*event.warning));
    if (event.input) object.raw_field("input", json_input_info(*event.input));
    if (event.device) object.raw_field("device", json_device(*event.device));
    if (event.route) object.raw_field("route", json_route(*event.route));
    if (event.impulse_detection) {
        object.raw_field("impulseDetection", json_impulse_detection(*event.impulse_detection));
    }
    if (event.alignment) object.raw_field("alignment", json_alignment(*event.alignment));
    if (event.verification) object.raw_field("verification", json_verification(*event.verification));
    if (event.output) object.raw_field("output", json_output_info(*event.output));
    if (event.sample_rate) object.number_field("sampleRate", *event.sample_rate);
    if (event.elapsed_seconds) object.number_field("elapsedSeconds", *event.elapsed_seconds);
    if (event.total_frames) object.integer_field("totalFrames", *event.total_frames);
    if (event.padding_seconds) object.number_field("paddingSeconds", *event.padding_seconds);
    if (event.marker_to_payload_silence_seconds) {
        object.number_field(
            "markerToPayloadSilenceSeconds",
            *event.marker_to_payload_silence_seconds);
    }
    object.string_field("message", event.message);
    return object.finish();
}

[[nodiscard]] CaptureEventHandler json_event_reporter(std::ostream& output) {
    return [&output](const CaptureEvent& event) {
        write_json_line(output, json_capture_event(event));
    };
}

[[nodiscard]] std::string json_capture_result(const CapturePassResult& result) {
    auto object = protocol_object("capture_result");
    object.raw_field("input", json_input_info(result.input));
    object.raw_field("output", json_output_info(result.output));
    object.raw_field("alignment", json_alignment(result.alignment));
    object.number_field("elapsedSeconds", result.elapsed.count());
    return object.finish();
}

[[nodiscard]] std::string json_test_result(const CaptureVerificationResult& result) {
    auto object = protocol_object("test_result");
    object.boolean_field("passed", result.passed());
    object.raw_field("input", json_input_info(result.input));
    object.raw_field("alignment", json_alignment(result.alignment));
    object.raw_field("verification", json_verification(result.verification));
    if (result.impulse_detection) {
        object.raw_field("impulseDetection", json_impulse_detection(*result.impulse_detection));
    } else {
        object.null_field("impulseDetection");
    }
    object.number_field("outputPeakDbfs", result.output_peak_dbfs);
    object.number_field("inputPeakDbfs", result.input_peak_dbfs);
    object.number_field("sampleRate", result.sample_rate);
    object.raw_field("warnings", json_array(
        result.warnings,
        [](const CaptureWarning warning) { return json_warning(warning); }));
    object.raw_field("failures", json_array(
        result.failures,
        [](const CaptureFailure failure) { return json_failure(failure); }));
    object.number_field("elapsedSeconds", result.elapsed.count());
    return object.finish();
}

[[nodiscard]] std::string json_error(
    const std::string_view category,
    const std::string_view code,
    const std::string_view message) {
    auto object = protocol_object("error");
    object.string_field("category", category);
    object.string_field("code", code);
    object.string_field("message", message);
    return object.finish();
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

[[nodiscard]] double parse_trim_db(
    const std::string& value,
    const std::string_view label,
    const double minimum,
    const double maximum) {
    const auto parsed = parse_finite_double(value, label);
    if (parsed < minimum || parsed > maximum) {
        throw std::invalid_argument(
            std::string(label) + " must be between " + invariant_number_text(minimum)
            + " and " + invariant_number_text(maximum) + " dB.");
    }
    return parsed;
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

[[nodiscard]] bool looks_like_option(const std::string_view argument) noexcept {
    // A leading double dash always denotes an option in this CLI. Single-dash
    // negative numbers remain valid separate values for gain options.
    return argument.starts_with("--")
        || argument == "-h"
        || argument == "-v"
        || argument == "-i"
        || argument == "-o";
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
    if (++index >= args.size() || looks_like_option(args[index])) {
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
        output << "Usage: capture-panel devices [--inputs|-i | --outputs|-o] [--json]\n";
    } else if (topic == "channels") {
        output << "Usage: capture-panel channels --driver <id> [--json]\n";
    } else if (topic == "test") {
        output
            << "Usage: capture-panel test --driver <id> --play-channel <spec> "
               "--record-channel <spec> [--sample-rate <"
            << invariant_number_text(constants::audio::minimum_supported_sample_rate) << ".."
            << invariant_number_text(constants::audio::maximum_supported_sample_rate)
            << ">] [--output-trim <"
            << invariant_number_text(constants::gain::output_minimum_db) << ".."
            << invariant_number_text(constants::gain::output_maximum_db)
            << ">] [--input-trim <"
            << invariant_number_text(constants::gain::input_minimum_db) << ".."
            << invariant_number_text(constants::gain::input_maximum_db)
            << ">] [--json] [--verbose]\n";
    } else if (topic == "run") {
        output
            << "Usage: capture-panel run --input <source.wav> --output <result.wav> "
               "--driver <id> --play-channel <spec> --record-channel <spec> "
               "[--bit-depth <16|24|32>] [--output-trim <"
            << invariant_number_text(constants::gain::output_minimum_db) << ".."
            << invariant_number_text(constants::gain::output_maximum_db)
            << ">] [--input-trim <"
            << invariant_number_text(constants::gain::input_minimum_db) << ".."
            << invariant_number_text(constants::gain::input_maximum_db)
            << ">] [--json] [--verbose]\n";
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
    if (command.json) {
        auto object = protocol_object("devices");
        object.raw_field("devices", json_array(
            visible_devices,
            [](const AudioDevice& device) { return json_device(device); }));
        write_json_line(output, object.finish());
        return;
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
    if (command.json) {
        const auto input_channels = dependencies.device_provider->channels(
            command.driver_id,
            ChannelDirection::input);
        const auto output_channels = dependencies.device_provider->channels(
            command.driver_id,
            ChannelDirection::output);
        auto object = protocol_object("channels");
        object.raw_field("device", json_device(device));
        object.raw_field("inputs", json_array(
            input_channels,
            [](const AudioChannel& channel) { return json_channel(channel); }));
        object.raw_field("outputs", json_array(
            output_channels,
            [](const AudioChannel& channel) { return json_channel(channel); }));
        write_json_line(output, object.finish());
        return;
    }

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
        command.json ? json_event_reporter(output) : event_reporter(command.verbose, output));

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
    const CapturePassOptions options{
        .playback_gain_db = command.output_trim_db,
        .recording_gain_db = command.input_trim_db,
    };
    const auto result = service.capture(
        configuration,
        options,
        dependencies.cancellation);

    if (command.json) {
        write_json_line(output, json_capture_result(result));
        return success_exit_code;
    }

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
        command.json ? json_event_reporter(output) : event_reporter(command.verbose, output));
    const CaptureRoute route{
        .driver_id = command.driver_id,
        .playback_channels = command.playback_channels,
        .record_channels = command.record_channels,
    };

    if (!command.json) {
        output << "Testing route\n"
               << "Driver: " << command.driver_id << '\n'
               << "Playback channels: " << join_channels(command.playback_channels) << '\n'
               << "Recording channels: " << join_channels(command.record_channels) << '\n';
    }
    const auto result = service.verify_setup(
        route,
        command.sample_rate,
        command.output_trim_db,
        command.input_trim_db,
        {},
        dependencies.cancellation);

    if (command.json) {
        write_json_line(output, json_test_result(result));
        return result.passed() ? success_exit_code : runtime_error_exit_code;
    }

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
            << "and modify it under GPLv3. Release bundles include the full terms in\n"
            << "licenses/GPL-3.0.txt and notices in licenses/THIRD_PARTY_NOTICES.md.\n";
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
    bool input_trim_seen = false;
    bool sample_rate_seen = false;
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
        } else if (argument == "--json") {
            parsed.json = true;
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
            parsed.output_trim_db = parse_trim_db(
                option_value(args, index, "--output-trim"),
                "--output-trim",
                constants::gain::output_minimum_db,
                constants::gain::output_maximum_db);
        } else if (matches_option(argument, "--input-trim")) {
            input_trim_seen = true;
            parsed.input_trim_db = parse_trim_db(
                option_value(args, index, "--input-trim"),
                "--input-trim",
                constants::gain::input_minimum_db,
                constants::gain::input_maximum_db);
        } else if (matches_option(argument, "--sample-rate")) {
            sample_rate_seen = true;
            const auto sample_rate = parse_finite_double(
                option_value(args, index, "--sample-rate"),
                "--sample-rate");
            if (sample_rate < constants::audio::minimum_supported_sample_rate
                || sample_rate > constants::audio::maximum_supported_sample_rate) {
                throw std::invalid_argument(
                    "--sample-rate must be between "
                    + invariant_number_text(constants::audio::minimum_supported_sample_rate)
                    + " and "
                    + invariant_number_text(constants::audio::maximum_supported_sample_rate)
                    + " Hz.");
            }
            parsed.sample_rate = sample_rate;
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
            || input_trim_seen || sample_rate_seen || parsed.verbose)) {
        throw std::invalid_argument("devices only accepts --inputs, --outputs, or --json.");
    }
    if (parsed.command == Command::channels) {
        require_option(!parsed.driver_id.empty(), "--driver", "channels");
        if (!parsed.playback_channels.empty() || !parsed.record_channels.empty()
            || !parsed.input_path.empty() || !parsed.output_path.empty()
            || parsed.bit_depth || output_trim_seen || parsed.verbose) {
            throw std::invalid_argument("channels only accepts a driver ID and --json.");
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
    if (input_trim_seen
        && parsed.command != Command::test
        && parsed.command != Command::run) {
        throw std::invalid_argument("--input-trim is only valid for test or run.");
    }
    if (sample_rate_seen && parsed.command != Command::test) {
        throw std::invalid_argument("--sample-rate is only valid for test.");
    }
    if (parsed.json
        && parsed.command != Command::devices
        && parsed.command != Command::channels
        && parsed.command != Command::test
        && parsed.command != Command::run) {
        throw std::invalid_argument("--json is only valid for devices, channels, test, or run.");
    }

    return parsed;
}

int run_cli(
    const std::vector<std::string>& args,
    Dependencies dependencies,
    std::ostream& output,
    std::ostream& error) {
    const auto json_requested = std::find(args.begin(), args.end(), "--json") != args.end();
    try {
        auto command = parse_arguments(args);
        // Treat the raw token as a mode request even if a malformed preceding
        // option consumed it as that option's value. This guarantees that an
        // invocation asking for JSON never leaks an unstructured stdout line.
        command.json = command.json || json_requested;
        if (json_requested && command.command == Command::help) {
            throw std::invalid_argument("--json cannot be combined with --help.");
        }
        return execute(command, dependencies, output);
    } catch (const std::invalid_argument& exception) {
        if (json_requested) {
            write_json_line(output, json_error("usage", "usage_error", exception.what()));
        } else {
            error << "Error: " << exception.what() << "\nTry 'capture-panel help'.\n";
        }
        return usage_error_exit_code;
    } catch (const CaptureError& exception) {
        if (json_requested) {
            write_json_line(
                output,
                json_error("capture", error_code_name(exception.code()), exception.what()));
        } else {
            error << "Capture error: " << exception.what() << '\n';
        }
        return runtime_error_exit_code;
    } catch (const std::exception& exception) {
        if (json_requested) {
            write_json_line(output, json_error("runtime", "runtime_error", exception.what()));
        } else {
            error << "Error: " << exception.what() << '\n';
        }
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

int report_startup_error(
    const std::string_view message,
    const bool json_requested,
    std::ostream& output,
    std::ostream& error) {
    if (json_requested) {
        write_json_line(output, json_error("runtime", "startup_error", message));
    } else {
        error << "Error: " << message << '\n';
    }
    return runtime_error_exit_code;
}

} // namespace capture_panel::cli
