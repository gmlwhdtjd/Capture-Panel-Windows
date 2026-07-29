#include "test_framework.hpp"

#include "cli.hpp"
#include "capture_panel/core/diagnostics.hpp"
#include "capture_panel/core/wav.hpp"
#include "capture_panel/fake/fake_backend.hpp"

#include <cctype>
#include <filesystem>
#include <locale>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace capture_panel;

namespace {

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

[[nodiscard]] std::string utf8(const std::u8string_view value) {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

class JsonSyntaxParser final {
public:
    explicit JsonSyntaxParser(const std::string_view value) : value_(value) {}

    [[nodiscard]] bool parse_document() {
        skip_whitespace();
        if (!parse_value()) return false;
        skip_whitespace();
        return position_ == value_.size();
    }

private:
    [[nodiscard]] bool parse_value() {
        if (position_ >= value_.size()) return false;
        switch (value_[position_]) {
        case '{': return parse_object();
        case '[': return parse_array();
        case '"': return parse_string();
        case 't': return parse_literal("true");
        case 'f': return parse_literal("false");
        case 'n': return parse_literal("null");
        default: return parse_number();
        }
    }

    [[nodiscard]] bool parse_object() {
        ++position_;
        skip_whitespace();
        if (consume('}')) return true;
        while (true) {
            if (!parse_string()) return false;
            skip_whitespace();
            if (!consume(':')) return false;
            skip_whitespace();
            if (!parse_value()) return false;
            skip_whitespace();
            if (consume('}')) return true;
            if (!consume(',')) return false;
            skip_whitespace();
        }
    }

    [[nodiscard]] bool parse_array() {
        ++position_;
        skip_whitespace();
        if (consume(']')) return true;
        while (true) {
            if (!parse_value()) return false;
            skip_whitespace();
            if (consume(']')) return true;
            if (!consume(',')) return false;
            skip_whitespace();
        }
    }

    [[nodiscard]] bool parse_string() {
        if (!consume('"')) return false;
        while (position_ < value_.size()) {
            const auto character = static_cast<unsigned char>(value_[position_++]);
            if (character == '"') return true;
            if (character < 0x20U) return false;
            if (character != '\\') continue;
            if (position_ >= value_.size()) return false;
            const auto escape = value_[position_++];
            if (escape == '"' || escape == '\\' || escape == '/'
                || escape == 'b' || escape == 'f' || escape == 'n'
                || escape == 'r' || escape == 't') {
                continue;
            }
            if (escape != 'u' || position_ + 4 > value_.size()) return false;
            for (int digit = 0; digit < 4; ++digit) {
                if (!std::isxdigit(static_cast<unsigned char>(value_[position_++]))) return false;
            }
        }
        return false;
    }

    [[nodiscard]] bool parse_number() {
        const auto start = position_;
        static_cast<void>(consume('-'));
        if (consume('0')) {
            if (position_ < value_.size()
                && std::isdigit(static_cast<unsigned char>(value_[position_]))) {
                return false;
            }
        } else {
            if (position_ >= value_.size()
                || value_[position_] < '1' || value_[position_] > '9') {
                position_ = start;
                return false;
            }
            while (position_ < value_.size()
                   && std::isdigit(static_cast<unsigned char>(value_[position_]))) {
                ++position_;
            }
        }
        if (consume('.')) {
            if (position_ >= value_.size()
                || !std::isdigit(static_cast<unsigned char>(value_[position_]))) {
                return false;
            }
            while (position_ < value_.size()
                   && std::isdigit(static_cast<unsigned char>(value_[position_]))) {
                ++position_;
            }
        }
        if (position_ < value_.size()
            && (value_[position_] == 'e' || value_[position_] == 'E')) {
            ++position_;
            if (position_ < value_.size()
                && (value_[position_] == '+' || value_[position_] == '-')) {
                ++position_;
            }
            if (position_ >= value_.size()
                || !std::isdigit(static_cast<unsigned char>(value_[position_]))) {
                return false;
            }
            while (position_ < value_.size()
                   && std::isdigit(static_cast<unsigned char>(value_[position_]))) {
                ++position_;
            }
        }
        return position_ > start;
    }

    [[nodiscard]] bool parse_literal(const std::string_view literal) {
        if (value_.substr(position_, literal.size()) != literal) return false;
        position_ += literal.size();
        return true;
    }

    [[nodiscard]] bool consume(const char expected) {
        if (position_ >= value_.size() || value_[position_] != expected) return false;
        ++position_;
        return true;
    }

    void skip_whitespace() {
        while (position_ < value_.size()) {
            const auto character = value_[position_];
            if (character != ' ' && character != '\t'
                && character != '\r' && character != '\n') {
                return;
            }
            ++position_;
        }
    }

    std::string_view value_;
    std::size_t position_ = 0;
};

[[nodiscard]] bool is_valid_utf8(const std::string_view value) {
    const auto continuation = [](const unsigned char byte) {
        return byte >= 0x80U && byte <= 0xBFU;
    };
    for (std::size_t offset = 0; offset < value.size();) {
        const auto remaining = value.size() - offset;
        const auto first = static_cast<unsigned char>(value[offset]);
        if (first <= 0x7FU) {
            ++offset;
            continue;
        }
        if (first >= 0xC2U && first <= 0xDFU) {
            if (remaining < 2
                || !continuation(static_cast<unsigned char>(value[offset + 1]))) return false;
            offset += 2;
            continue;
        }
        if (remaining >= 3) {
            const auto second = static_cast<unsigned char>(value[offset + 1]);
            const auto third = static_cast<unsigned char>(value[offset + 2]);
            if (((first == 0xE0U && second >= 0xA0U && second <= 0xBFU)
                 || (first >= 0xE1U && first <= 0xECU && continuation(second))
                 || (first == 0xEDU && second >= 0x80U && second <= 0x9FU)
                 || (first >= 0xEEU && first <= 0xEFU && continuation(second)))
                && continuation(third)) {
                offset += 3;
                continue;
            }
        }
        if (remaining >= 4) {
            const auto second = static_cast<unsigned char>(value[offset + 1]);
            const auto third = static_cast<unsigned char>(value[offset + 2]);
            const auto fourth = static_cast<unsigned char>(value[offset + 3]);
            if (((first == 0xF0U && second >= 0x90U && second <= 0xBFU)
                 || (first >= 0xF1U && first <= 0xF3U && continuation(second))
                 || (first == 0xF4U && second >= 0x80U && second <= 0x8FU))
                && continuation(third) && continuation(fourth)) {
                offset += 4;
                continue;
            }
        }
        return false;
    }
    return true;
}

[[nodiscard]] std::vector<std::string_view> non_empty_lines(const std::string& value) {
    std::vector<std::string_view> lines;
    for (std::size_t start = 0; start < value.size();) {
        const auto newline = value.find('\n', start);
        const auto end = newline == std::string::npos ? value.size() : newline;
        auto line = std::string_view(value).substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty()) lines.push_back(line);
        if (newline == std::string::npos) break;
        start = newline + 1;
    }
    return lines;
}

[[nodiscard]] bool valid_protocol_jsonl(const std::string& value) {
    if (value.empty() || value.back() != '\n') return false;
    for (std::size_t start = 0; start < value.size();) {
        const auto newline = value.find('\n', start);
        if (newline == std::string::npos) return false;
        auto line = std::string_view(value).substr(start, newline - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) return false;
        if (!is_valid_utf8(line)
            || !JsonSyntaxParser(line).parse_document()
            || line.find("\"protocol\":\"capture-panel/1\"") == std::string_view::npos) {
            return false;
        }
        start = newline + 1;
    }
    return true;
}

class CountingStringBuffer final : public std::stringbuf {
public:
    int sync() override {
        ++sync_count;
        return std::stringbuf::sync();
    }

    std::size_t sync_count = 0;
};

class CommaDecimalPoint final : public std::numpunct<char> {
protected:
    [[nodiscard]] char do_decimal_point() const override { return ','; }
};

class EscapingDeviceProvider final : public IAudioDeviceProvider {
public:
    [[nodiscard]] std::vector<AudioDevice> devices() const override {
        auto status = std::string("invalid byte: ");
        status.push_back(static_cast<char>(0xFF));
        return {{
            .id = "escaped:device",
            .name = utf8(u8"아나그램 \"장치\"\n"),
            .input_channels = 1,
            .output_channels = 1,
            .sample_rate = 48'000.5,
            .available = true,
            .status = std::move(status),
        }};
    }

    [[nodiscard]] AudioDevice device(const std::string&) const override {
        return devices().front();
    }

    [[nodiscard]] std::vector<AudioChannel> channels(
        const std::string&,
        const ChannelDirection direction) const override {
        return {{
            .index = 1,
            .name = direction == ChannelDirection::input
                ? utf8(u8"입력 \"1\"")
                : utf8(u8"출력 \\ 1"),
        }};
    }

    void set_sample_rate(const std::string&, double) override {}
};

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
        "--input-trim", "-3.25",
        "--json",
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
    CP_REQUIRE_NEAR(command.input_trim_db, -3.25, 0.000001);
    CP_REQUIRE(!command.sample_rate.has_value());
    CP_REQUIRE(command.json);
    CP_REQUIRE(command.verbose);
}

CP_TEST_CASE("CLI parses test routing and positional channels driver") {
    const auto test_command = cli::parse_arguments({
        "test",
        "--driver=fake:loopback",
        "--play-channel=1",
        "--record-channel=8",
        "--sample-rate=44100",
        "--input-trim=-2.5",
        "--json",
    });
    CP_REQUIRE(test_command.command == cli::Command::test);
    CP_REQUIRE(test_command.record_channels == std::vector<std::uint32_t>({8}));
    CP_REQUIRE(test_command.sample_rate.has_value());
    CP_REQUIRE_NEAR(*test_command.sample_rate, 44'100.0, 0.000001);
    CP_REQUIRE_NEAR(test_command.input_trim_db, -2.5, 0.000001);
    CP_REQUIRE(test_command.json);

    const auto channels_command = cli::parse_arguments({"channels", "fake:loopback"});
    CP_REQUIRE(channels_command.command == cli::Command::channels);
    CP_REQUIRE(channels_command.driver_id == "fake:loopback");

    const auto input_devices = cli::parse_arguments({"devices", "-i"});
    CP_REQUIRE(input_devices.inputs_only);
    const auto output_devices = cli::parse_arguments({"devices", "-o"});
    CP_REQUIRE(output_devices.outputs_only);
}

CP_TEST_CASE("CLI accepts trim boundaries and rejects values outside them") {
    const auto minimums = cli::parse_arguments({
        "test",
        "--driver", "fake:loopback",
        "--play-channel", "1",
        "--record-channel", "1",
        "--output-trim", "-24",
        "--input-trim", "-18",
    });
    CP_REQUIRE_NEAR(minimums.output_trim_db, -24.0, 0.000001);
    CP_REQUIRE_NEAR(minimums.input_trim_db, -18.0, 0.000001);

    const auto maximums = cli::parse_arguments({
        "test",
        "--driver", "fake:loopback",
        "--play-channel", "1",
        "--record-channel", "1",
        "--output-trim", "0",
        "--input-trim", "12",
    });
    CP_REQUIRE_NEAR(maximums.output_trim_db, 0.0, 0.000001);
    CP_REQUIRE_NEAR(maximums.input_trim_db, 12.0, 0.000001);

    for (const auto& [option, value] : std::vector<std::pair<std::string, std::string>>{
             {"--output-trim", "-24.0001"},
             {"--output-trim", "0.0001"},
             {"--input-trim", "-18.0001"},
             {"--input-trim", "12.0001"},
         }) {
        auto rejected = false;
        try {
            (void)cli::parse_arguments({
                "test",
                "--driver", "fake:loopback",
                "--play-channel", "1",
                "--record-channel", "1",
                option, value,
            });
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        CP_REQUIRE(rejected);
    }
}

CP_TEST_CASE("CLI accepts only the documented setup sample-rate range") {
    for (const auto& value : {std::string("1000"), std::string("768000")}) {
        const auto parsed = cli::parse_arguments({
            "test",
            "--driver", "fake:loopback",
            "--play-channel", "1",
            "--record-channel", "1",
            "--sample-rate", value,
        });
        CP_REQUIRE(parsed.sample_rate.has_value());
    }

    for (const auto& value : {std::string("999.9"), std::string("768000.1")}) {
        auto rejected = false;
        try {
            static_cast<void>(cli::parse_arguments({
                "test",
                "--driver", "fake:loopback",
                "--play-channel", "1",
                "--record-channel", "1",
                "--sample-rate", value,
            }));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        CP_REQUIRE(rejected);
    }
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

    bool run_sample_rate = false;
    try {
        (void)cli::parse_arguments({
            "run", "--driver", "fake:loopback", "--play-channel", "1",
            "--record-channel", "1", "--input", "a.wav", "--output", "b.wav",
            "--sample-rate", "48000",
        });
    } catch (const std::invalid_argument&) {
        run_sample_rate = true;
    }
    CP_REQUIRE(run_sample_rate);

    bool invalid_sample_rate = false;
    try {
        (void)cli::parse_arguments({
            "test", "--driver", "fake:loopback", "--play-channel", "1",
            "--record-channel", "1", "--sample-rate", "0",
        });
    } catch (const std::invalid_argument&) {
        invalid_sample_rate = true;
    }
    CP_REQUIRE(invalid_sample_rate);
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

CP_TEST_CASE("CLI devices and channels JSON is valid UTF-8 escaped and locale independent") {
    auto provider = std::make_shared<EscapingDeviceProvider>();
    const cli::Dependencies dependencies{.device_provider = provider};

    std::ostringstream device_output;
    device_output.imbue(std::locale(
        std::locale::classic(),
        new CommaDecimalPoint));
    std::ostringstream device_error;
    CP_REQUIRE(cli::run_cli(
        {"devices", "--json"},
        dependencies,
        device_output,
        device_error) == 0);
    const auto devices_json = device_output.str();
    CP_REQUIRE(device_error.str().empty());
    CP_REQUIRE(valid_protocol_jsonl(devices_json));
    CP_REQUIRE(non_empty_lines(devices_json).size() == 1);
    CP_REQUIRE(devices_json.find("\"type\":\"devices\"") != std::string::npos);
    CP_REQUIRE(devices_json.find("\"sampleRate\":48000.5") != std::string::npos);
    CP_REQUIRE(devices_json.find("48000,5") == std::string::npos);
    CP_REQUIRE(devices_json.find(utf8(u8"아나그램")) != std::string::npos);
    CP_REQUIRE(devices_json.find("\\\"장치\\\"") != std::string::npos);
    CP_REQUIRE(devices_json.find("\\n") != std::string::npos);
    CP_REQUIRE(devices_json.find("\\uFFFD") != std::string::npos);
    CP_REQUIRE(devices_json.find(static_cast<char>(0xFF)) == std::string::npos);

    std::ostringstream channel_output;
    std::ostringstream channel_error;
    CP_REQUIRE(cli::run_cli(
        {"channels", "--driver", "escaped:device", "--json"},
        dependencies,
        channel_output,
        channel_error) == 0);
    const auto channels_json = channel_output.str();
    CP_REQUIRE(channel_error.str().empty());
    CP_REQUIRE(valid_protocol_jsonl(channels_json));
    CP_REQUIRE(non_empty_lines(channels_json).size() == 1);
    CP_REQUIRE(channels_json.find("\"type\":\"channels\"") != std::string::npos);
    CP_REQUIRE(channels_json.find("\"inputs\":[") != std::string::npos);
    CP_REQUIRE(channels_json.find("\"outputs\":[") != std::string::npos);
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

CP_TEST_CASE("CLI setup test emits versioned JSON events and result") {
    auto backend = std::make_shared<fake::FakeAudioBackend>(fake::FakeBackendOptions{
        .progress_block_frames = 20'000,
    });
    std::ostringstream output;
    std::ostringstream error;

    const auto exit_code = cli::run_cli(
        {
            "test",
            "--driver", "fake:loopback",
            "--play-channel", "1",
            "--record-channel", "1",
            "--sample-rate", "44100",
            "--input-trim", "-6",
            "--json",
            "--verbose",
        },
        {.device_provider = backend, .capture_backend = backend},
        output,
        error);

    const auto jsonl = output.str();
    CP_REQUIRE(exit_code == 0);
    CP_REQUIRE(error.str().empty());
    CP_REQUIRE(valid_protocol_jsonl(jsonl));
    CP_REQUIRE(jsonl.find("Testing route\n") == std::string::npos);
    CP_REQUIRE(jsonl.find("Recording:") == std::string::npos);
    CP_REQUIRE(jsonl.find("\"type\":\"event\"") != std::string::npos);
    CP_REQUIRE(jsonl.find("\"event\":\"recording_progress\"") != std::string::npos);
    CP_REQUIRE(jsonl.find("\"type\":\"test_result\"") != std::string::npos);
    CP_REQUIRE(jsonl.find("\"passed\":true") != std::string::npos);
    CP_REQUIRE(jsonl.find("\"sampleRate\":44100") != std::string::npos);
    const auto input_peak_key = std::string("\"inputPeakDbfs\":");
    const auto input_peak_offset = jsonl.find(input_peak_key);
    CP_REQUIRE(input_peak_offset != std::string::npos);
    CP_REQUIRE_NEAR(
        std::stod(jsonl.substr(input_peak_offset + input_peak_key.size())),
        -18.0,
        0.05);
}

CP_TEST_CASE("CLI setup test JSON reports adjusted peak without hiding raw clipping") {
    auto backend = std::make_shared<fake::FakeAudioBackend>(fake::FakeBackendOptions{
        .loopback_gain_db = 12.041199826559248,
    });
    std::ostringstream output;
    std::ostringstream error;

    const auto exit_code = cli::run_cli(
        {
            "test",
            "--driver", "fake:loopback",
            "--play-channel", "1",
            "--record-channel", "1",
            "--input-trim", "-12",
            "--json",
        },
        {.device_provider = backend, .capture_backend = backend},
        output,
        error);

    const auto jsonl = output.str();
    CP_REQUIRE(exit_code == 1);
    CP_REQUIRE(error.str().empty());
    CP_REQUIRE(valid_protocol_jsonl(jsonl));
    CP_REQUIRE(jsonl.find("\"type\":\"test_result\"") != std::string::npos);
    CP_REQUIRE(jsonl.find("\"passed\":false") != std::string::npos);
    CP_REQUIRE(jsonl.find("\"code\":\"digital_clipping\"") != std::string::npos);
    const auto input_peak_key = std::string("\"inputPeakDbfs\":");
    const auto input_peak_offset = jsonl.find(input_peak_key);
    CP_REQUIRE(input_peak_offset != std::string::npos);
    CP_REQUIRE_NEAR(
        std::stod(jsonl.substr(input_peak_offset + input_peak_key.size())),
        -12.0,
        0.05);
}

CP_TEST_CASE("CLI flushes each JSON progress line") {
    auto backend = std::make_shared<fake::FakeAudioBackend>(fake::FakeBackendOptions{
        .progress_block_frames = 50'000,
    });
    CountingStringBuffer output_buffer;
    std::ostream output(&output_buffer);
    std::ostringstream error;

    CP_REQUIRE(cli::run_cli(
        {
            "test",
            "--driver", "fake:loopback",
            "--play-channel", "1",
            "--record-channel", "1",
            "--json",
        },
        {.device_provider = backend, .capture_backend = backend},
        output,
        error) == 0);

    const auto jsonl = output_buffer.str();
    const auto lines = non_empty_lines(jsonl);
    CP_REQUIRE(error.str().empty());
    CP_REQUIRE(valid_protocol_jsonl(jsonl));
    CP_REQUIRE(count_occurrences(jsonl, "\"event\":\"recording_progress\"") >= 2);
    CP_REQUIRE(output_buffer.sync_count == lines.size());
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

CP_TEST_CASE("CLI run JSON emits events and capture result and applies input trim") {
    TemporaryCliFiles files;
    const AudioBuffer source{
        .sample_rate = 1'000.0,
        .channel_count = 1,
        .samples = {0.40F, -0.40F, 0.20F, -0.20F},
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
            "--input-trim", "-6.020599913279624",
            "--json",
        },
        {.device_provider = backend, .capture_backend = backend},
        output,
        error);

    const auto jsonl = output.str();
    CP_REQUIRE(exit_code == 0);
    CP_REQUIRE(error.str().empty());
    CP_REQUIRE(std::filesystem::exists(files.output));
    CP_REQUIRE(valid_protocol_jsonl(jsonl));
    CP_REQUIRE(jsonl.find("Capture complete\n") == std::string::npos);
    CP_REQUIRE(jsonl.find("\"type\":\"event\"") != std::string::npos);
    CP_REQUIRE(jsonl.find("\"type\":\"capture_result\"") != std::string::npos);

    const auto recorded = read_wav(files.output);
    CP_REQUIRE(recorded.audio.channel_count == 1);
    CP_REQUIRE(recorded.audio.samples.size() == source.samples.size());
    CP_REQUIRE_NEAR(recorded.audio.samples.front(), 0.20, 0.00001);
}

CP_TEST_CASE("CLI help version license and usage errors do not require audio services") {
    const cli::Dependencies no_dependencies;
    std::ostringstream output;
    std::ostringstream error;

    CP_REQUIRE(cli::run_cli({"help", "run"}, no_dependencies, output, error) == 0);
    CP_REQUIRE(output.str().find("--bit-depth") != std::string::npos);
    CP_REQUIRE(output.str().find("--output-trim <-24..0>") != std::string::npos);
    CP_REQUIRE(output.str().find("--input-trim <-18..12>") != std::string::npos);

    output.str("");
    output.clear();
    CP_REQUIRE(cli::run_cli({"help", "test"}, no_dependencies, output, error) == 0);
    CP_REQUIRE(output.str().find("--sample-rate <1000..768000>") != std::string::npos);

    output.str("");
    output.clear();
    CP_REQUIRE(cli::run_cli({"version"}, no_dependencies, output, error) == 0);
    CP_REQUIRE(output.str() == "capture-panel 0.2.2\n");

    output.str("");
    output.clear();
    CP_REQUIRE(cli::run_cli({"license"}, no_dependencies, output, error) == 0);
    CP_REQUIRE(output.str().find("GPL") != std::string::npos);
    CP_REQUIRE(!warning_message(CaptureWarning::marker_evidence_low).empty());
    CP_REQUIRE(!failure_message(CaptureFailure::digital_clipping).empty());
    CP_REQUIRE(error_code_name(ErrorCode::capture_cancelled) == "capture_cancelled");
    CP_REQUIRE(warning_name(CaptureWarning::verification_ambiguous) == "verification_ambiguous");
    CP_REQUIRE(failure_name(CaptureFailure::verification_signal_missing)
        == "verification_signal_missing");
    CP_REQUIRE(stage_name(CaptureStage::output_writing) == "output_writing");
    CP_REQUIRE(event_type_name(CaptureEventType::recording_progress) == "recording_progress");

    output.str("");
    output.clear();
    CP_REQUIRE(cli::run_cli({"run", "--bit-depth", "20"}, no_dependencies, output, error) == 2);
    CP_REQUIRE(error.str().find("16, 24, or 32") != std::string::npos);
}

CP_TEST_CASE("CLI JSON mode frames usage runtime and malformed-option errors") {
    const cli::Dependencies no_dependencies;

    std::ostringstream usage_output;
    std::ostringstream usage_error;
    CP_REQUIRE(cli::run_cli(
        {"run", "--json", "--bit-depth", "20"},
        no_dependencies,
        usage_output,
        usage_error) == 2);
    CP_REQUIRE(usage_error.str().empty());
    CP_REQUIRE(valid_protocol_jsonl(usage_output.str()));
    CP_REQUIRE(non_empty_lines(usage_output.str()).size() == 1);
    CP_REQUIRE(usage_output.str().find("\"type\":\"error\"") != std::string::npos);
    CP_REQUIRE(usage_output.str().find("\"category\":\"usage\"") != std::string::npos);
    CP_REQUIRE(usage_output.str().find("\"code\":\"usage_error\"") != std::string::npos);

    std::ostringstream help_output;
    std::ostringstream help_error;
    CP_REQUIRE(cli::run_cli(
        {"test", "--json", "--help"},
        no_dependencies,
        help_output,
        help_error) == 2);
    CP_REQUIRE(help_error.str().empty());
    CP_REQUIRE(valid_protocol_jsonl(help_output.str()));
    CP_REQUIRE(help_output.str().find("Usage: capture-panel") == std::string::npos);

    std::ostringstream runtime_output;
    std::ostringstream runtime_error;
    CP_REQUIRE(cli::run_cli(
        {"devices", "--json"},
        no_dependencies,
        runtime_output,
        runtime_error) == 1);
    CP_REQUIRE(runtime_error.str().empty());
    CP_REQUIRE(valid_protocol_jsonl(runtime_output.str()));
    CP_REQUIRE(runtime_output.str().find("\"category\":\"runtime\"") != std::string::npos);

    auto backend = std::make_shared<fake::FakeAudioBackend>();
    const cli::Dependencies fake_dependencies{
        .device_provider = backend,
        .capture_backend = backend,
    };
    std::ostringstream consumed_output;
    std::ostringstream consumed_error;
    CP_REQUIRE(cli::run_cli(
        {
            "test",
            "--driver", "--json",
            "--play-channel", "1",
            "--record-channel", "1",
        },
        fake_dependencies,
        consumed_output,
        consumed_error) == 2);
    CP_REQUIRE(consumed_error.str().empty());
    CP_REQUIRE(valid_protocol_jsonl(consumed_output.str()));
    CP_REQUIRE(consumed_output.str().find("\"category\":\"usage\"") != std::string::npos);
    CP_REQUIRE(consumed_output.str().find("--driver requires a value") != std::string::npos);
    CP_REQUIRE(consumed_output.str().find("Testing route\n") == std::string::npos);

    auto invalid_startup_message = std::string("startup \"failure\": ");
    invalid_startup_message.push_back(static_cast<char>(0xFF));
    std::ostringstream startup_output;
    std::ostringstream startup_error;
    CP_REQUIRE(cli::report_startup_error(
        invalid_startup_message,
        true,
        startup_output,
        startup_error) == 1);
    CP_REQUIRE(startup_error.str().empty());
    CP_REQUIRE(valid_protocol_jsonl(startup_output.str()));
    CP_REQUIRE(startup_output.str().find("\"category\":\"runtime\"")
        != std::string::npos);
    CP_REQUIRE(startup_output.str().find("\"code\":\"startup_error\"")
        != std::string::npos);
    CP_REQUIRE(startup_output.str().find("\\\"failure\\\"") != std::string::npos);
    CP_REQUIRE(startup_output.str().find("\\uFFFD") != std::string::npos);
}

CP_TEST_CASE("CLI JSON reports trim range violations before configuring audio") {
    const cli::Dependencies no_dependencies;
    for (const auto& [option, value] : std::vector<std::pair<std::string, std::string>>{
             {"--output-trim", "1000"},
             {"--input-trim", "1000"},
         }) {
        std::ostringstream output;
        std::ostringstream error;
        const auto exit_code = cli::run_cli(
            {
                "test",
                "--driver", "fake:loopback",
                "--play-channel", "1",
                "--record-channel", "1",
                option, value,
                "--json",
            },
            no_dependencies,
            output,
            error);

        CP_REQUIRE(exit_code == 2);
        CP_REQUIRE(error.str().empty());
        CP_REQUIRE(valid_protocol_jsonl(output.str()));
        CP_REQUIRE(non_empty_lines(output.str()).size() == 1);
        CP_REQUIRE(output.str().find("\"category\":\"usage\"") != std::string::npos);
        CP_REQUIRE(output.str().find(option) != std::string::npos);
    }
}

CP_TEST_CASE("CLI JSON rejects sample rates outside the documented range") {
    std::ostringstream output;
    std::ostringstream error;

    const auto exit_code = cli::run_cli(
        {
            "test",
            "--driver", "fake:loopback",
            "--play-channel", "1",
            "--record-channel", "1",
            "--sample-rate", "1e308",
            "--json",
        },
        {},
        output,
        error);

    CP_REQUIRE(exit_code == 2);
    CP_REQUIRE(error.str().empty());
    CP_REQUIRE(valid_protocol_jsonl(output.str()));
    CP_REQUIRE(non_empty_lines(output.str()).size() == 1);
    CP_REQUIRE(output.str().find("\"category\":\"usage\"") != std::string::npos);
    CP_REQUIRE(output.str().find("--sample-rate must be between 1000 and 768000 Hz")
        != std::string::npos);
    CP_REQUIRE(output.str().find("\"event\":\"recording_progress\"")
        == std::string::npos);
}

CP_TEST_CASE("CLI passes a cancellation token into capture commands") {
    auto backend = std::make_shared<fake::FakeAudioBackend>();
    auto cancellation = std::make_shared<CancellationToken>();
    cancellation->cancel();
    const cli::Dependencies dependencies{
        .device_provider = backend,
        .capture_backend = backend,
        .cancellation = cancellation,
    };
    std::ostringstream output;
    std::ostringstream error;

    const auto exit_code = cli::run_cli(
        {
            "test",
            "--driver", "fake:loopback",
            "--play-channel", "1",
            "--record-channel", "1",
        },
        dependencies,
        output,
        error);

    CP_REQUIRE(exit_code != 0);
    CP_REQUIRE(error.str().find("cancelled") != std::string::npos);

    std::ostringstream json_output;
    std::ostringstream json_error;
    const auto json_exit_code = cli::run_cli(
        {
            "test",
            "--driver", "fake:loopback",
            "--play-channel", "1",
            "--record-channel", "1",
            "--json",
        },
        dependencies,
        json_output,
        json_error);

    CP_REQUIRE(json_exit_code == 1);
    CP_REQUIRE(json_error.str().empty());
    CP_REQUIRE(valid_protocol_jsonl(json_output.str()));
    CP_REQUIRE(json_output.str().find("\"type\":\"error\"") != std::string::npos);
    CP_REQUIRE(json_output.str().find("\"category\":\"capture\"") != std::string::npos);
    CP_REQUIRE(json_output.str().find("\"code\":\"capture_cancelled\"")
        != std::string::npos);
}
