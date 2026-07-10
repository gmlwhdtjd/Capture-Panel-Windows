#include "capture_panel/fake/fake_backend.hpp"

#include "capture_panel/core/errors.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <utility>

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
    return static_cast<float>(std::pow(10.0, gain_db / 20.0));
}

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
    validate_channels(
        request.route.playback_channels,
        options_.output_channels,
        "playback");
    validate_channels(
        request.route.record_channels,
        options_.input_channels,
        "recording");

    if (!std::isfinite(request.playback.sample_rate) || request.playback.sample_rate <= 0.0) {
        throw CaptureError(ErrorCode::unsupported_sample_rate, "Playback sample rate must be positive.");
    }
    if (std::abs(request.playback.sample_rate - options_.sample_rate) > 0.5) {
        throw CaptureError(
            ErrorCode::unsupported_sample_rate,
            "Playback sample rate does not match the fake device sample rate.");
    }
    if (request.playback.channel_count == 0 || request.playback.frame_count() <= 0) {
        throw CaptureError(ErrorCode::validation_failed, "Playback audio must contain at least one frame and channel.");
    }
    if (request.playback.samples.size() % request.playback.channel_count != 0) {
        throw CaptureError(ErrorCode::validation_failed, "Playback audio is not frame-aligned.");
    }
    if (!std::isfinite(request.padding_seconds) || request.padding_seconds < 0.0) {
        throw CaptureError(ErrorCode::validation_failed, "Capture padding cannot be negative.");
    }

    const auto padding_frame_count = request.padding_seconds * request.playback.sample_rate;
    if (padding_frame_count >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        throw CaptureError(ErrorCode::validation_failed, "Fake capture padding is too large.");
    }
    const auto padding_frames = static_cast<std::int64_t>(
        std::llround(padding_frame_count));
    const auto playback_frames = request.playback.frame_count();
    if (padding_frames > (std::numeric_limits<std::int64_t>::max() - playback_frames) / 2) {
        throw CaptureError(ErrorCode::validation_failed, "Fake capture duration is too large.");
    }
    const auto total_frames = padding_frames + playback_frames + padding_frames;
    const auto record_channels = static_cast<std::uint32_t>(request.route.record_channels.size());

    AudioBuffer recorded{
        .sample_rate = request.playback.sample_rate,
        .channel_count = record_channels,
        .samples = std::vector<float>(checked_sample_count(total_frames, record_channels), 0.0F),
    };

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

    const auto gain = linear_gain(options_.loopback_gain_db);
    for (std::int64_t block_start = 0; block_start < total_frames;
         block_start += options_.progress_block_frames) {
        ensure_not_cancelled();
        const auto block_end = block_start + std::min(
            options_.progress_block_frames,
            total_frames - block_start);

        for (std::int64_t record_frame = block_start; record_frame < block_end; ++record_frame) {
            const auto playback_frame = record_frame - padding_frames - options_.latency_frames;
            if (playback_frame < 0 || playback_frame >= playback_frames) continue;

            const auto mapped_channels = std::min<std::size_t>(
                {request.playback.channel_count,
                 request.route.playback_channels.size(),
                 record_channels});
            for (std::size_t channel = 0; channel < mapped_channels; ++channel) {
                const auto source_index =
                    static_cast<std::size_t>(playback_frame) * request.playback.channel_count + channel;
                const auto destination_index =
                    static_cast<std::size_t>(record_frame) * record_channels + channel;
                recorded.samples[destination_index] = request.playback.samples[source_index] * gain;
            }
        }

        report_progress(block_end);
    }

    return {.recorded = std::move(recorded), .pre_pad_frames = padding_frames};
}

void FakeAudioBackend::validate_device_id(const std::string& id) const {
    if (id != loopback_device_id) {
        throw CaptureError(ErrorCode::device_not_found, "Audio driver not found: " + id);
    }
}

void FakeAudioBackend::validate_channels(
    const std::vector<std::uint32_t>& channels,
    std::uint32_t available,
    std::string_view direction) const {
    if (channels.empty()) {
        throw CaptureError(
            ErrorCode::invalid_channel_specification,
            std::string(direction) + " channels cannot be empty.");
    }

    for (const auto channel : channels) {
        if (channel == 0 || channel > available) {
            std::ostringstream message;
            message << "Invalid " << direction << " channel " << channel
                    << "; expected a channel in the range 1-" << available << '.';
            throw CaptureError(ErrorCode::invalid_channel_specification, message.str());
        }
    }
}

} // namespace capture_panel::fake
