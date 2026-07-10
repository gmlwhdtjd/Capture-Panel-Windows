#include "test_framework.hpp"

#include "capture_panel/core/errors.hpp"
#include "capture_panel/core/wav.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace capture_panel;

namespace {

class TemporaryWav {
public:
    explicit TemporaryWav(const std::string& suffix) {
        static std::uint64_t sequence = 0;
        path_ = std::filesystem::temp_directory_path()
            / ("capture-panel-wav-test-" + std::to_string(++sequence) + '-' + suffix + ".wav");
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    ~TemporaryWav() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void append_u16(std::vector<std::uint8_t>& bytes, const std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void append_u32(std::vector<std::uint8_t>& bytes, const std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

void append_fourcc(std::vector<std::uint8_t>& bytes, const char (&value)[5]) {
    for (std::size_t index = 0; index < 4; ++index) {
        bytes.push_back(static_cast<std::uint8_t>(value[index]));
    }
}

void set_u32(std::vector<std::uint8_t>& bytes, const std::size_t offset, const std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xFFU);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    bytes[offset + 3] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
}

void write_bytes(const std::filesystem::path& path, const std::span<const std::uint8_t> bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!stream) throw std::runtime_error("test WAV fixture could not be written");
}

[[nodiscard]] std::vector<std::uint8_t> make_float_wav_with_unknown_chunk() {
    std::vector<std::uint8_t> bytes;
    append_fourcc(bytes, "RIFF");
    append_u32(bytes, 0); // Patched below.
    append_fourcc(bytes, "WAVE");

    append_fourcc(bytes, "JUNK");
    append_u32(bytes, 3);
    bytes.insert(bytes.end(), {1, 2, 3, 0}); // RIFF chunks are word padded.

    append_fourcc(bytes, "fmt ");
    append_u32(bytes, 16);
    append_u16(bytes, 3); // WAVE_FORMAT_IEEE_FLOAT
    append_u16(bytes, 1);
    append_u32(bytes, 48'000);
    append_u32(bytes, 192'000);
    append_u16(bytes, 4);
    append_u16(bytes, 32);

    append_fourcc(bytes, "data");
    append_u32(bytes, 12);
    for (const auto sample : std::array<float, 3>{0.0F, -0.5F, 1.25F}) {
        append_u32(bytes, std::bit_cast<std::uint32_t>(sample));
    }

    set_u32(bytes, 4, static_cast<std::uint32_t>(bytes.size() - 8));
    return bytes;
}

[[nodiscard]] std::vector<std::uint8_t> make_extensible_pcm_wav() {
    std::vector<std::uint8_t> bytes;
    append_fourcc(bytes, "RIFF");
    append_u32(bytes, 0); // Patched below.
    append_fourcc(bytes, "WAVE");

    append_fourcc(bytes, "fmt ");
    append_u32(bytes, 40);
    append_u16(bytes, 0xFFFE); // WAVE_FORMAT_EXTENSIBLE
    append_u16(bytes, 1);
    append_u32(bytes, 48'000);
    append_u32(bytes, 192'000);
    append_u16(bytes, 4);
    append_u16(bytes, 32); // 32-bit container
    append_u16(bytes, 22); // extension size
    append_u16(bytes, 24); // 24 valid bits, left aligned
    append_u32(bytes, 0);  // channel mask
    append_u32(bytes, 1);  // KSDATAFORMAT_SUBTYPE_PCM Data1
    append_u16(bytes, 0);
    append_u16(bytes, 0x0010);
    bytes.insert(bytes.end(), {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71});

    append_fourcc(bytes, "data");
    append_u32(bytes, 8);
    append_u32(bytes, std::bit_cast<std::uint32_t>(std::int32_t{-4'194'304} * 256));
    append_u32(bytes, std::bit_cast<std::uint32_t>(std::int32_t{2'097'152} * 256));

    set_u32(bytes, 4, static_cast<std::uint32_t>(bytes.size() - 8));
    return bytes;
}

} // namespace

CP_TEST_CASE("WAV PCM writer and reader round-trip supported bit depths") {
    const AudioBuffer source{
        48'000.0,
        2,
        {-1.0F, -0.75F, -0.5F, -0.25F, 0.0F, 0.25F, 0.5F, 0.999F},
    };

    for (const auto [depth, tolerance] : std::array{
             std::pair{AudioBitDepth::pcm16, 1.0 / 32'768.0},
             std::pair{AudioBitDepth::pcm24, 1.0 / 8'388'608.0},
             std::pair{AudioBitDepth::pcm32, 1.0 / 2'147'483'648.0},
         }) {
        TemporaryWav file(std::to_string(static_cast<int>(depth)));
        write_wav(file.path(), source, depth);
        const auto result = read_wav(file.path());

        CP_REQUIRE_NEAR(result.format.sample_rate, 48'000.0, 1.0e-9);
        CP_REQUIRE(result.format.channel_count == 2);
        CP_REQUIRE(result.format.bit_depth == depth);
        CP_REQUIRE(result.format.total_frames == 4);
        CP_REQUIRE_NEAR(result.format.duration_seconds(), 4.0 / 48'000.0, 1.0e-12);
        CP_REQUIRE(result.audio.samples.size() == source.samples.size());
        for (std::size_t index = 0; index < source.samples.size(); ++index) {
            CP_REQUIRE_NEAR(result.audio.samples[index], source.samples[index], tolerance + 1.0e-9);
        }
    }
}

CP_TEST_CASE("WAV writer saturates out-of-range samples and maps NaN to silence") {
    TemporaryWav file("saturation");
    const std::vector<float> source{-2.0F, 2.0F, std::nanf("")};
    write_wav(file.path(), source, 44'100.0, 1, AudioBitDepth::pcm16);
    const auto result = read_wav(file.path());
    CP_REQUIRE_NEAR(result.audio.samples[0], -1.0, 1.0e-9);
    CP_REQUIRE_NEAR(result.audio.samples[1], 32'767.0 / 32'768.0, 1.0e-9);
    CP_REQUIRE_NEAR(result.audio.samples[2], 0.0, 1.0e-9);
}

CP_TEST_CASE("WAV writer pads odd-sized PCM24 data chunks") {
    TemporaryWav file("pcm24-padding");
    const std::vector<float> source{0.25F};
    write_wav(file.path(), source, 48'000.0, 1, AudioBitDepth::pcm24);
    const auto result = read_wav(file.path());
    CP_REQUIRE(result.format.total_frames == 1);
    CP_REQUIRE_NEAR(result.audio.samples[0], 0.25, 1.0 / 8'388'608.0);
    CP_REQUIRE(std::filesystem::file_size(file.path()) == 48);
}

CP_TEST_CASE("WAV reader accepts IEEE float32 and skips unknown chunks") {
    TemporaryWav file("float");
    const auto bytes = make_float_wav_with_unknown_chunk();
    write_bytes(file.path(), bytes);

    const auto result = read_wav(file.path());
    CP_REQUIRE(result.format.bit_depth == AudioBitDepth::pcm32);
    CP_REQUIRE(result.format.channel_count == 1);
    CP_REQUIRE(result.format.total_frames == 3);
    CP_REQUIRE_NEAR(result.audio.samples[0], 0.0, 1.0e-9);
    CP_REQUIRE_NEAR(result.audio.samples[1], -0.5, 1.0e-9);
    // IEEE float values are already normalized representation; no destructive clamping occurs.
    CP_REQUIRE_NEAR(result.audio.samples[2], 1.25, 1.0e-9);
}

CP_TEST_CASE("WAV reader accepts extensible PCM and honors valid bits") {
    TemporaryWav file("extensible");
    const auto bytes = make_extensible_pcm_wav();
    write_bytes(file.path(), bytes);

    const auto result = read_wav(file.path());
    CP_REQUIRE(result.format.bit_depth == AudioBitDepth::pcm32);
    CP_REQUIRE(result.format.total_frames == 2);
    CP_REQUIRE_NEAR(result.audio.samples[0], -0.5, 1.0e-9);
    CP_REQUIRE_NEAR(result.audio.samples[1], 0.25, 1.0e-9);
}

CP_TEST_CASE("WAV reader reports missing files with the public error code") {
    TemporaryWav file("missing");
    bool failed = false;
    try {
        static_cast<void>(read_wav(file.path()));
    } catch (const CaptureError& error) {
        failed = error.code() == ErrorCode::file_not_found;
    }
    CP_REQUIRE(failed);
}

CP_TEST_CASE("WAV writer validates format dimensions") {
    TemporaryWav file("invalid");
    bool bad_channels = false;
    try {
        write_wav(file.path(), std::vector<float>{0.0F}, 48'000.0, 0);
    } catch (const CaptureError& error) {
        bad_channels = error.code() == ErrorCode::wav_write;
    }
    CP_REQUIRE(bad_channels);

    bool partial_frame = false;
    try {
        write_wav(file.path(), std::vector<float>{0.0F}, 48'000.0, 2);
    } catch (const CaptureError& error) {
        partial_frame = error.code() == ErrorCode::wav_write;
    }
    CP_REQUIRE(partial_frame);

    bool bad_rate = false;
    try {
        write_wav(file.path(), std::vector<float>{0.0F}, 48'000.5, 1);
    } catch (const CaptureError& error) {
        bad_rate = error.code() == ErrorCode::unsupported_sample_rate;
    }
    CP_REQUIRE(bad_rate);
}
