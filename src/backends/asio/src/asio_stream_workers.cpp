#include "asio_stream_workers.hpp"

#include "capture_panel/core/audio.hpp"
#include "capture_panel/core/constants.hpp"
#include "capture_panel/core/errors.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace capture_panel::asio {
namespace {

constexpr std::size_t worker_chunk_frames = default_audio_chunk_frames;
static_assert(std::atomic<AsioStreamFailure>::is_always_lock_free);
static_assert(std::atomic_bool::is_always_lock_free);

[[nodiscard]] std::size_t checked_samples(
    const std::size_t frames,
    const std::size_t channels,
    const char* purpose) {
    if (frames == 0 || channels == 0
        || frames > std::numeric_limits<std::size_t>::max() / channels) {
        throw CaptureError(
            ErrorCode::validation_failed,
            std::string("Invalid ASIO ") + purpose + " streaming dimensions.");
    }
    return frames * channels;
}

[[nodiscard]] std::size_t ring_frames(
    const double sample_rate,
    const std::size_t asio_buffer_frames) {
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0 || asio_buffer_frames == 0) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "Invalid ASIO streaming buffer configuration.");
    }
    if (asio_buffer_frames > std::numeric_limits<std::size_t>::max() / 8) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "The ASIO streaming buffer size is too large.");
    }
    const auto half_second = std::ceil(sample_rate * 0.5);
    if (!std::isfinite(half_second)
        || half_second > static_cast<double>(std::numeric_limits<std::size_t>::max())) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "The ASIO streaming prebuffer is too large.");
    }
    return std::max(
        asio_buffer_frames * 8,
        static_cast<std::size_t>(half_second));
}

void validate_plan(const CapturePassPlaybackPlan& plan) {
    if (!plan.source.valid() || !std::isfinite(plan.sample_rate())
        || plan.sample_rate() <= 0.0 || plan.channel_count() == 0
        || plan.source_start_frame < 0 || plan.playback_frame_count <= 0
        || !std::isfinite(plan.playback_gain)) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "The ASIO playback stream descriptor is invalid.");
    }
    const auto source_frames = plan.source.format().total_frames;
    if (source_frames <= 0
        || plan.source_start_frame
            > std::numeric_limits<std::int64_t>::max() - source_frames
        || plan.playback_frame_count != plan.source_start_frame + source_frames) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "The ASIO playback stream length does not match its source descriptor.");
    }
    if (!std::is_sorted(plan.marker_frames.begin(), plan.marker_frames.end())
        || std::adjacent_find(plan.marker_frames.begin(), plan.marker_frames.end())
            != plan.marker_frames.end()
        || std::any_of(
            plan.marker_frames.begin(),
            plan.marker_frames.end(),
            [&](const auto frame) {
                return frame < 0 || frame >= plan.source_start_frame;
            })) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "The ASIO playback marker positions are invalid.");
    }
}

} // namespace

AsioStreamWorkers::AsioStreamWorkers(
    CapturePassPlaybackPlan playback_plan,
    const std::uint32_t record_channel_count,
    const std::int64_t expected_record_frames,
    const std::size_t asio_buffer_frames,
    std::optional<std::filesystem::path> scratch_file_prefix)
    : playback_plan_(std::move(playback_plan)),
      record_channel_count_(record_channel_count),
      expected_record_frames_(expected_record_frames),
      playback_ring_(checked_samples(
          ring_frames(playback_plan_.sample_rate(), asio_buffer_frames),
          playback_plan_.channel_count(),
          "playback ring")),
      recording_ring_(checked_samples(
          ring_frames(playback_plan_.sample_rate(), asio_buffer_frames),
          record_channel_count,
          "recording ring")),
      temporary_file_(scratch_file_prefix) {
    validate_plan(playback_plan_);
    if (record_channel_count_ == 0 || expected_record_frames_ <= 0) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "The ASIO recording stream descriptor is invalid.");
    }
}

AsioStreamWorkers::~AsioStreamWorkers() {
    request_stop();
    join();
}

void AsioStreamWorkers::start() {
    if (started_) {
        throw CaptureError(
            ErrorCode::backend_failure,
            "ASIO stream workers cannot be started more than once.");
    }
    started_ = true;
    try {
        playback_thread_ = std::thread([this] { produce_playback(); });
        writer_thread_ = std::thread([this] { write_recording(); });
    } catch (...) {
        request_stop();
        join();
        throw;
    }
}

void AsioStreamWorkers::wait_until_playback_ready(
    const std::size_t minimum_frames,
    const std::chrono::steady_clock::duration timeout,
    const std::shared_ptr<CancellationToken>& cancellation) {
    const auto requested_frames = static_cast<std::size_t>(std::min<std::int64_t>(
        playback_plan_.playback_frame_count,
        static_cast<std::int64_t>(std::max<std::size_t>(1, minimum_frames))));
    const auto requested_samples = checked_samples(
        requested_frames,
        playback_plan_.channel_count(),
        "playback prebuffer");
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (playback_ring_.available_to_read() < requested_samples) {
        if (cancellation && cancellation->is_cancelled()) {
            request_stop();
            throw CaptureError(ErrorCode::capture_cancelled, "ASIO capture was cancelled.");
        }
        switch (failure()) {
        case AsioStreamFailure::source_failure:
            throw CaptureError(
                ErrorCode::source_stream_failure,
                "The source audio could not be streamed for ASIO playback.");
        case AsioStreamFailure::writer_failure:
            throw CaptureError(
                ErrorCode::recording_write_failure,
                "The temporary ASIO recording could not be opened for writing.");
        case AsioStreamFailure::recording_overflow:
            throw CaptureError(
                ErrorCode::recording_stream_overflow,
                "The ASIO recording stream overflowed before startup.");
        case AsioStreamFailure::playback_underflow:
            throw CaptureError(
                ErrorCode::playback_stream_underflow,
                "The ASIO playback stream underflowed before startup.");
        case AsioStreamFailure::none:
            break;
        }
        if (playback_producer_finished_.load(std::memory_order_acquire)
            || std::chrono::steady_clock::now() >= deadline) {
            publish_failure(AsioStreamFailure::playback_underflow);
            throw CaptureError(
                ErrorCode::playback_stream_underflow,
                "The ASIO playback stream could not fill its initial buffer.");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void AsioStreamWorkers::request_stop() noexcept {
    stop_requested_.store(true, std::memory_order_release);
}

void AsioStreamWorkers::join() noexcept {
    if (joined_) return;
    if (playback_thread_.joinable()) playback_thread_.join();
    if (writer_thread_.joinable()) writer_thread_.join();
    joined_ = true;
}

void AsioStreamWorkers::publish_failure(const AsioStreamFailure value) noexcept {
    if (value == AsioStreamFailure::none) return;
    auto expected = AsioStreamFailure::none;
    static_cast<void>(failure_.compare_exchange_strong(
        expected,
        value,
        std::memory_order_acq_rel,
        std::memory_order_acquire));
}

void AsioStreamWorkers::fail_playback_underflow() noexcept {
    publish_failure(AsioStreamFailure::playback_underflow);
}

void AsioStreamWorkers::fail_recording_overflow() noexcept {
    publish_failure(AsioStreamFailure::recording_overflow);
}

AsioStreamFailure AsioStreamWorkers::failure() const noexcept {
    return failure_.load(std::memory_order_acquire);
}

void AsioStreamWorkers::mark_playback_consumed() noexcept {
    playback_consumed_.store(true, std::memory_order_release);
}

bool AsioStreamWorkers::playback_consumed() const noexcept {
    return playback_consumed_.load(std::memory_order_acquire);
}

void AsioStreamWorkers::mark_recording_producer_finished() noexcept {
    recording_producer_finished_.store(true, std::memory_order_release);
}

bool AsioStreamWorkers::recording_producer_finished() const noexcept {
    return recording_producer_finished_.load(std::memory_order_acquire);
}

bool AsioStreamWorkers::writer_finished() const noexcept {
    return writer_finished_.load(std::memory_order_acquire);
}

bool AsioStreamWorkers::should_stop() const noexcept {
    return stop_requested_.load(std::memory_order_acquire) || failed();
}

bool AsioStreamWorkers::write_playback(const std::span<const float> samples) noexcept {
    std::size_t offset = 0;
    while (offset < samples.size()) {
        if (should_stop()) return false;
        const auto written = playback_ring_.write(samples.subspan(offset));
        if (written == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
            offset += written;
        }
    }
    return true;
}

void AsioStreamWorkers::produce_playback() noexcept {
    try {
        const auto channels = static_cast<std::size_t>(playback_plan_.channel_count());
        std::vector<float> buffer(checked_samples(
            worker_chunk_frames, channels, "producer chunk"));
        const auto marker_amplitude = static_cast<float>(linear_from_dbfs(
            constants::alignment::impulse_level_dbfs)) * playback_plan_.playback_gain;

        std::int64_t preamble_frame = 0;
        auto marker = playback_plan_.marker_frames.begin();
        while (preamble_frame < playback_plan_.source_start_frame) {
            if (should_stop()) return;
            const auto frames = static_cast<std::size_t>(std::min<std::int64_t>(
                playback_plan_.source_start_frame - preamble_frame,
                static_cast<std::int64_t>(worker_chunk_frames)));
            auto samples = std::span<float>(buffer).first(frames * channels);
            std::fill(samples.begin(), samples.end(), 0.0F);
            const auto chunk_end = preamble_frame + static_cast<std::int64_t>(frames);
            while (marker != playback_plan_.marker_frames.end() && *marker < chunk_end) {
                const auto local_frame = static_cast<std::size_t>(*marker - preamble_frame);
                for (std::size_t channel = 0; channel < channels; ++channel) {
                    samples[local_frame * channels + channel] = marker_amplitude;
                }
                ++marker;
            }
            if (!write_playback(samples)) return;
            preamble_frame = chunk_end;
        }

        auto reader = playback_plan_.source.make_reader();
        auto remaining = playback_plan_.source.format().total_frames;
        while (remaining > 0) {
            if (should_stop()) return;
            const auto requested = static_cast<std::size_t>(std::min<std::int64_t>(
                remaining,
                static_cast<std::int64_t>(worker_chunk_frames)));
            auto samples = std::span<float>(buffer).first(requested * channels);
            const auto read = reader->read_frames(samples);
            if (read <= 0 || read > static_cast<std::int64_t>(requested)) {
                throw CaptureError(
                    ErrorCode::source_stream_failure,
                    "The ASIO playback source ended unexpectedly.");
            }
            auto read_samples = samples.first(static_cast<std::size_t>(read) * channels);
            if (playback_plan_.playback_gain != 1.0F) {
                for (auto& sample : read_samples) sample *= playback_plan_.playback_gain;
            }
            if (!write_playback(read_samples)) return;
            remaining -= read;
        }
        playback_producer_finished_.store(true, std::memory_order_release);
    } catch (...) {
        publish_failure(AsioStreamFailure::source_failure);
        playback_producer_finished_.store(true, std::memory_order_release);
    }
}

void AsioStreamWorkers::write_recording() noexcept {
    try {
        const auto channels = static_cast<std::size_t>(record_channel_count_);
        std::vector<float> buffer(checked_samples(
            worker_chunk_frames, channels, "recording writer chunk"));
        std::int64_t written_frames = 0;
        float peak = 0.0F;

        while (true) {
            auto available = recording_ring_.available_to_read();
            if (available > 0) {
                const auto count = std::min(available, buffer.size());
                const auto aligned = count - (count % channels);
                if (aligned == 0) {
                    if (recording_producer_finished()) {
                        throw CaptureError(
                            ErrorCode::recording_write_failure,
                            "The ASIO recording ring ended on a partial frame.");
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                const auto read = recording_ring_.read(
                    std::span<float>(buffer).first(aligned));
                if (read == 0 || read % channels != 0) {
                    throw CaptureError(
                        ErrorCode::recording_write_failure,
                        "The ASIO recording ring returned an invalid block.");
                }
                const auto samples = std::span<const float>(buffer).first(read);
                for (const auto sample : samples) {
                    if (!std::isfinite(sample)) {
                        throw CaptureError(
                            ErrorCode::recording_write_failure,
                            "The ASIO driver recorded NaN or infinity.");
                    }
                    peak = std::max(peak, std::abs(sample));
                }
                temporary_file_.write(samples);
                written_frames += static_cast<std::int64_t>(read / channels);
                if (written_frames > expected_record_frames_) {
                    throw CaptureError(
                        ErrorCode::recording_write_failure,
                        "The ASIO recording exceeded its declared frame count.");
                }
                continue;
            }

            if (recording_producer_finished()) {
                // The acquire above observes the final exact callback write.
                // Re-read the cursor before closing so the last block cannot
                // be skipped even when the flag and cursor become visible
                // close together.
                if (recording_ring_.available_to_read() > 0) continue;
                if (written_frames != expected_record_frames_) {
                    throw CaptureError(
                        ErrorCode::recording_write_failure,
                        "The temporary ASIO recording has an incomplete frame count.");
                }
                temporary_file_.close();
                raw_peak_ = peak;
                writer_finished_.store(true, std::memory_order_release);
                return;
            }
            if (should_stop()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } catch (...) {
        publish_failure(AsioStreamFailure::writer_failure);
    }
}

Float32AudioAsset AsioStreamWorkers::take_recorded_asset() {
    if (!joined_ || failed() || !writer_finished() || asset_transferred_) {
        throw CaptureError(
            ErrorCode::recording_write_failure,
            "The completed ASIO recording is not available.");
    }

    // Construct the new RAII owner before disarming our file owner. If asset
    // construction throws, AsioTemporaryFloatFile still deletes the scratch.
    auto asset = Float32AudioAsset::from_temporary_file(
        temporary_file_.path(),
        playback_plan_.sample_rate(),
        record_channel_count_,
        expected_record_frames_,
        raw_peak_,
        true);
    static_cast<void>(temporary_file_.release_ownership());
    asset_transferred_ = true;
    return asset;
}

} // namespace capture_panel::asio
