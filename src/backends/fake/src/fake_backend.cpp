#include "capture_panel/fake/fake_backend.hpp"

#include "capture_panel/core/channels.hpp"
#include "capture_panel/core/constants.hpp"
#include "capture_panel/core/errors.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace capture_panel::fake {
namespace {

[[nodiscard]] std::size_t checked_sample_count(
    std::int64_t frames,
    std::uint32_t channels) {
    if (frames < 0 || channels == 0) {
        throw CaptureError(ErrorCode::validation_failed, "Invalid fake audio buffer dimensions.");
    }

    const auto frame_count = static_cast<std::uint64_t>(frames);
    if (frame_count > std::numeric_limits<std::size_t>::max() / channels) {
        throw CaptureError(ErrorCode::validation_failed, "Fake audio buffer is too large.");
    }
    return static_cast<std::size_t>(frame_count * channels);
}

[[nodiscard]] float linear_gain(double gain_db) {
    const auto gain = static_cast<float>(std::pow(10.0, gain_db / 20.0));
    if (!std::isfinite(gain)) {
        throw CaptureError(ErrorCode::validation_failed, "Fake loopback gain is too large.");
    }
    return gain;
}

class ScratchFloatFile final {
public:
    explicit ScratchFloatFile(
        const std::optional<std::filesystem::path>& requested_prefix) {
        static std::atomic_uint64_t sequence{0};
        const auto prefix = requested_prefix.value_or(
            std::filesystem::temp_directory_path() / "capture-panel-recording.");
        for (int attempt = 0; attempt < 32; ++attempt) {
            const auto token = std::to_string(GetCurrentProcessId()) + "-"
                + std::to_string(
                    std::chrono::high_resolution_clock::now().time_since_epoch().count())
                + "-" + std::to_string(
                    sequence.fetch_add(1, std::memory_order_relaxed));
            path_ = prefix;
            path_ += token + ".f32";
            constexpr std::size_t maximum_windows_component_length = 255;
            if (path_.filename().native().size() > maximum_windows_component_length) {
                path_ = prefix.parent_path()
                    / (".capture-panel.tmp." + token + ".f32");
            }
            handle_ = CreateFileW(
                path_.c_str(),
                GENERIC_WRITE,
                0,
                nullptr,
                CREATE_NEW,
                FILE_ATTRIBUTE_TEMPORARY,
                nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) return;
            const auto error = GetLastError();
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) break;
        }
        throw CaptureError(
            ErrorCode::recording_write_failure,
            "Unable to create a unique fake recording scratch file.");
    }

    ScratchFloatFile(const ScratchFloatFile&) = delete;
    ScratchFloatFile& operator=(const ScratchFloatFile&) = delete;

    ~ScratchFloatFile() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        if (!remove_on_destroy_) return;
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    void write(const std::span<const float> samples) {
        auto bytes = std::as_bytes(samples);
        while (!bytes.empty()) {
            const auto chunk_size = static_cast<DWORD>(std::min<std::size_t>(
                bytes.size(), std::numeric_limits<DWORD>::max()));
            DWORD written = 0;
            if (!WriteFile(handle_, bytes.data(), chunk_size, &written, nullptr)
                || written != chunk_size) {
                throw CaptureError(
                    ErrorCode::recording_write_failure,
                    "Unable to write fake recording scratch file.");
            }
            bytes = bytes.subspan(written);
        }
    }

    void close() {
        const auto flushed = FlushFileBuffers(handle_) != FALSE;
        const auto closed = CloseHandle(handle_) != FALSE;
        handle_ = INVALID_HANDLE_VALUE;
        if (!flushed || !closed) {
            throw CaptureError(
                ErrorCode::recording_write_failure,
                "Unable to close fake recording scratch file.");
        }
    }

    void release() noexcept { remove_on_destroy_ = false; }

private:
    std::filesystem::path path_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    bool remove_on_destroy_ = true;
};

} // namespace

FakeAudioBackend::FakeAudioBackend(FakeBackendOptions options)
    : options_(options) {
    if (!std::isfinite(options_.sample_rate) || options_.sample_rate <= 0.0) {
        throw CaptureError(ErrorCode::unsupported_sample_rate, "Fake sample rate must be positive.");
    }
    if (options_.input_channels == 0 || options_.output_channels == 0) {
        throw CaptureError(ErrorCode::validation_failed, "Fake device must expose input and output channels.");
    }
    if (options_.latency_frames < 0) {
        throw CaptureError(ErrorCode::validation_failed, "Fake loopback latency cannot be negative.");
    }
    if (!std::isfinite(options_.loopback_gain_db)) {
        throw CaptureError(ErrorCode::validation_failed, "Fake loopback gain must be finite.");
    }
    if (options_.progress_block_frames <= 0) {
        throw CaptureError(ErrorCode::validation_failed, "Fake progress block size must be positive.");
    }
}

void FakeAudioBackend::set_latency_frames(std::int64_t frames) {
    if (frames < 0) {
        throw CaptureError(ErrorCode::validation_failed, "Fake loopback latency cannot be negative.");
    }
    options_.latency_frames = frames;
}

void FakeAudioBackend::set_loopback_gain_db(double gain_db) {
    if (!std::isfinite(gain_db)) {
        throw CaptureError(ErrorCode::validation_failed, "Fake loopback gain must be finite.");
    }
    options_.loopback_gain_db = gain_db;
}

std::vector<AudioDevice> FakeAudioBackend::devices() const {
    return {{
        .id = std::string(loopback_device_id),
        .name = "Fake Loopback",
        .input_channels = options_.input_channels,
        .output_channels = options_.output_channels,
        .sample_rate = options_.sample_rate,
        .available = true,
        .status = "development backend",
    }};
}

AudioDevice FakeAudioBackend::device(const std::string& id) const {
    validate_device_id(id);
    return devices().front();
}

std::vector<AudioChannel> FakeAudioBackend::channels(
    const std::string& id,
    ChannelDirection direction) const {
    validate_device_id(id);
    const auto count = direction == ChannelDirection::input
        ? options_.input_channels
        : options_.output_channels;
    const auto prefix = direction == ChannelDirection::input ? "Input " : "Output ";

    std::vector<AudioChannel> result;
    result.reserve(count);
    for (std::uint32_t offset = 0; offset < count; ++offset) {
        const auto index = offset + 1;
        result.push_back({.index = index, .name = prefix + std::to_string(index)});
    }
    return result;
}

void FakeAudioBackend::set_sample_rate(const std::string& id, double sample_rate) {
    validate_device_id(id);
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0) {
        throw CaptureError(ErrorCode::unsupported_sample_rate, "Sample rate must be positive.");
    }

    // The fake device follows the source rate just as an ASIO device would after
    // a successful sample-rate change. It starts at 48 kHz by default.
    options_.sample_rate = sample_rate;
}

RawAudioCaptureResult FakeAudioBackend::capture(const RawAudioCaptureRequest& request) {
    validate_device_id(request.route.driver_id);
    validate_playback_channels(
        request.route.playback_channels,
        options_.output_channels);
    validate_record_channels(
        request.route.record_channels,
        options_.input_channels);

    const auto& playback = request.playback_plan;
    if (!playback.source.valid()
        || !std::isfinite(playback.sample_rate()) || playback.sample_rate() <= 0.0) {
        throw CaptureError(ErrorCode::unsupported_sample_rate, "Playback sample rate must be positive.");
    }
    if (std::abs(playback.sample_rate() - options_.sample_rate) > 0.5) {
        throw CaptureError(
            ErrorCode::unsupported_sample_rate,
            "Playback sample rate does not match the fake device sample rate.");
    }
    if (playback.channel_count() == 0 || playback.playback_frame_count <= 0
        || playback.source_start_frame < 0
        || playback.source_start_frame > playback.playback_frame_count
        || playback.source.format().total_frames
            != playback.playback_frame_count - playback.source_start_frame
        || !std::isfinite(playback.playback_gain)) {
        throw CaptureError(ErrorCode::validation_failed, "Playback audio must contain at least one frame and channel.");
    }
    if (!std::is_sorted(playback.marker_frames.begin(), playback.marker_frames.end())
        || std::adjacent_find(
            playback.marker_frames.begin(), playback.marker_frames.end())
                != playback.marker_frames.end()
        || std::any_of(
            playback.marker_frames.begin(), playback.marker_frames.end(),
            [&](const std::int64_t frame) {
                return frame < 0 || frame >= playback.source_start_frame;
            })) {
        throw CaptureError(ErrorCode::validation_failed, "Playback marker positions are invalid.");
    }
    if (!std::isfinite(request.padding_seconds) || request.padding_seconds < 0.0) {
        throw CaptureError(ErrorCode::validation_failed, "Capture padding cannot be negative.");
    }

    const auto padding_frame_count = request.padding_seconds * playback.sample_rate();
    if (padding_frame_count >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        throw CaptureError(ErrorCode::validation_failed, "Fake capture padding is too large.");
    }
    const auto padding_frames = static_cast<std::int64_t>(
        std::llround(padding_frame_count));
    const auto playback_frames = playback.playback_frame_count;
    if (padding_frames > (std::numeric_limits<std::int64_t>::max() - playback_frames) / 2) {
        throw CaptureError(ErrorCode::validation_failed, "Fake capture duration is too large.");
    }
    const auto total_frames = padding_frames + playback_frames + padding_frames;
    const auto record_channels = static_cast<std::uint32_t>(request.route.record_channels.size());

    const auto report_progress = [&](std::int64_t completed) {
        if (request.progress) request.progress(completed, total_frames);
    };
    const auto ensure_not_cancelled = [&] {
        if (request.cancellation && request.cancellation->is_cancelled()) {
            throw CaptureError(ErrorCode::capture_cancelled, "Fake capture was cancelled.");
        }
    };

    ensure_not_cancelled();
    report_progress(0);

    auto source_reader = playback.source.make_reader();
    constexpr std::size_t preferred_chunk_samples = 1U << 20U;
    const auto source_chunk_frames = std::max<std::size_t>(
        1,
        std::min<std::size_t>(
            default_audio_chunk_frames,
            preferred_chunk_samples / playback.channel_count()));
    std::vector<float> source_chunk(
        checked_sample_count(
            static_cast<std::int64_t>(source_chunk_frames), playback.channel_count()));
    std::int64_t source_chunk_start = 0;
    std::int64_t source_chunk_count = 0;
    auto load_source_chunk = [&](const std::int64_t source_frame) {
        if (source_frame >= source_chunk_start
            && source_frame < source_chunk_start + source_chunk_count) {
            return;
        }
        if (source_frame != source_chunk_start + source_chunk_count) {
            throw CaptureError(
                ErrorCode::source_stream_failure,
                "Fake playback source was requested out of sequence.");
        }
        source_chunk_start = source_frame;
        source_chunk_count = 0;
        const auto remaining = playback.source.format().total_frames - source_frame;
        const auto requested = static_cast<std::size_t>(std::min<std::int64_t>(
            remaining, static_cast<std::int64_t>(source_chunk_frames)));
        while (source_chunk_count < static_cast<std::int64_t>(requested)) {
            ensure_not_cancelled();
            const auto sample_offset = static_cast<std::size_t>(source_chunk_count)
                * playback.channel_count();
            const auto read = source_reader->read_frames(
                std::span<float>(source_chunk).subspan(
                    sample_offset,
                    (requested - static_cast<std::size_t>(source_chunk_count))
                        * playback.channel_count()));
            if (read <= 0
                || read > static_cast<std::int64_t>(requested) - source_chunk_count) {
                throw CaptureError(
                    ErrorCode::source_stream_failure,
                    "Fake playback source ended before its declared frame count.");
            }
            source_chunk_count += read;
        }
        for (const auto sample : std::span<const float>(source_chunk).first(
                 requested * playback.channel_count())) {
            if (!std::isfinite(sample)) {
                throw CaptureError(
                    ErrorCode::source_stream_failure,
                    "Fake playback source contains NaN or infinity.");
            }
        }
    };

    ScratchFloatFile scratch(request.scratch_file_prefix);

    const auto loopback_gain = linear_gain(options_.loopback_gain_db);
    const auto source_gain = playback.playback_gain * loopback_gain;
    if (!std::isfinite(source_gain)) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "Combined fake playback gain is too large.");
    }
    const auto marker_gain = static_cast<float>(std::pow(
        10.0, constants::alignment::impulse_level_dbfs / 20.0))
        * source_gain;
    const auto bounded_block_frames = std::min<std::int64_t>(
        {options_.progress_block_frames,
         static_cast<std::int64_t>(default_audio_chunk_frames),
         std::max<std::int64_t>(
             1,
             static_cast<std::int64_t>(preferred_chunk_samples / record_channels))});
    std::vector<float> record_block(checked_sample_count(
        bounded_block_frames, record_channels));
    float raw_peak = 0.0F;
    for (std::int64_t block_start = 0; block_start < total_frames;
         block_start += bounded_block_frames) {
        ensure_not_cancelled();
        const auto block_end = block_start + std::min(
            bounded_block_frames,
            total_frames - block_start);
        const auto block_frames = block_end - block_start;
        std::fill(
            record_block.begin(),
            record_block.begin() + static_cast<std::ptrdiff_t>(
                checked_sample_count(block_frames, record_channels)),
            0.0F);

        for (std::int64_t record_frame = block_start; record_frame < block_end; ++record_frame) {
            if (record_frame < padding_frames) continue;
            const auto frame_after_padding = record_frame - padding_frames;
            if (frame_after_padding < options_.latency_frames) continue;
            const auto playback_frame = frame_after_padding - options_.latency_frames;
            if (playback_frame >= playback_frames) continue;

            const auto mapped_channels = std::min<std::size_t>(
                {playback.channel_count(),
                 request.route.playback_channels.size(),
                 record_channels});
            const auto block_frame = record_frame - block_start;
            const auto is_marker = std::binary_search(
                playback.marker_frames.begin(),
                playback.marker_frames.end(),
                playback_frame);
            const auto source_frame = playback_frame - playback.source_start_frame;
            if (source_frame >= 0) load_source_chunk(source_frame);
            for (std::size_t channel = 0; channel < mapped_channels; ++channel) {
                const auto destination_index =
                    static_cast<std::size_t>(block_frame) * record_channels + channel;
                float sample = 0.0F;
                if (is_marker) {
                    sample = marker_gain;
                } else if (source_frame >= 0) {
                    const auto source_offset = static_cast<std::size_t>(
                        source_frame - source_chunk_start) * playback.channel_count();
                    sample = source_chunk[source_offset + channel] * source_gain;
                }
                if (!std::isfinite(sample)) {
                    throw CaptureError(
                        ErrorCode::source_stream_failure,
                        "Fake playback produced NaN or infinity.");
                }
                record_block[destination_index] = sample;
                raw_peak = std::max(raw_peak, std::abs(sample));
            }
        }

        const auto block_samples = checked_sample_count(block_frames, record_channels);
        scratch.write(std::span<const float>(record_block).first(block_samples));

        report_progress(block_end);
    }

    // Recording latency can move the tail outside the captured input window,
    // but the playback producer still has to consume and validate the complete
    // source independently of that latency.
    auto decoded_source_frames = source_chunk_start + source_chunk_count;
    while (decoded_source_frames < playback.source.format().total_frames) {
        load_source_chunk(decoded_source_frames);
        decoded_source_frames = source_chunk_start + source_chunk_count;
    }

    scratch.close();
    ensure_not_cancelled();

    auto recorded = Float32AudioAsset::from_temporary_file(
        scratch.path(),
        playback.sample_rate(),
        record_channels,
        total_frames,
        raw_peak);
    scratch.release();
    return {.recorded = std::move(recorded), .pre_pad_frames = padding_frames};
}

void FakeAudioBackend::validate_device_id(const std::string& id) const {
    if (id != loopback_device_id) {
        throw CaptureError(ErrorCode::device_not_found, "Audio driver not found: " + id);
    }
}

} // namespace capture_panel::fake
