#pragma once

#include "asio_spsc_ring.hpp"
#include "asio_temporary_float_file.hpp"
#include "capture_panel/core/streaming.hpp"
#include "capture_panel/core/types.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <thread>

namespace capture_panel::asio {

enum class AsioStreamFailure : std::uint8_t {
    none,
    playback_underflow,
    recording_overflow,
    source_failure,
    writer_failure,
};

/// Owns the two bounded stream rings and their non-real-time producer/writer.
/// The ASIO callback only accesses the rings and atomic publication methods.
class AsioStreamWorkers final {
public:
    AsioStreamWorkers(
        CapturePassPlaybackPlan playback_plan,
        std::uint32_t record_channel_count,
        std::int64_t expected_record_frames,
        std::size_t asio_buffer_frames,
        std::optional<std::filesystem::path> scratch_file_prefix);
    ~AsioStreamWorkers();

    AsioStreamWorkers(const AsioStreamWorkers&) = delete;
    AsioStreamWorkers& operator=(const AsioStreamWorkers&) = delete;

    void start();
    void wait_until_playback_ready(
        std::size_t minimum_frames,
        std::chrono::steady_clock::duration timeout,
        const std::shared_ptr<CancellationToken>& cancellation);

    /// Stops workers that could otherwise wait on a ring after the driver has
    /// stopped, then joins both threads. Safe to call repeatedly.
    void request_stop() noexcept;
    void join() noexcept;

    [[nodiscard]] SpscRing<float>& playback_ring() noexcept { return playback_ring_; }
    [[nodiscard]] SpscRing<float>& recording_ring() noexcept { return recording_ring_; }

    void fail_playback_underflow() noexcept;
    void fail_recording_overflow() noexcept;
    [[nodiscard]] AsioStreamFailure failure() const noexcept;
    [[nodiscard]] bool failed() const noexcept {
        return failure() != AsioStreamFailure::none;
    }

    void mark_playback_consumed() noexcept;
    [[nodiscard]] bool playback_consumed() const noexcept;
    void mark_recording_producer_finished() noexcept;
    [[nodiscard]] bool recording_producer_finished() const noexcept;
    [[nodiscard]] bool writer_finished() const noexcept;

    [[nodiscard]] Float32AudioAsset take_recorded_asset();

private:
    void publish_failure(AsioStreamFailure failure) noexcept;
    [[nodiscard]] bool should_stop() const noexcept;
    [[nodiscard]] bool write_playback(std::span<const float> samples) noexcept;
    void produce_playback() noexcept;
    void write_recording() noexcept;

    CapturePassPlaybackPlan playback_plan_;
    std::uint32_t record_channel_count_ = 0;
    std::int64_t expected_record_frames_ = 0;
    SpscRing<float> playback_ring_;
    SpscRing<float> recording_ring_;
    AsioTemporaryFloatFile temporary_file_;

    std::thread playback_thread_;
    std::thread writer_thread_;
    std::atomic<AsioStreamFailure> failure_{AsioStreamFailure::none};
    std::atomic_bool stop_requested_{false};
    std::atomic_bool playback_producer_finished_{false};
    std::atomic_bool playback_consumed_{false};
    std::atomic_bool recording_producer_finished_{false};
    std::atomic_bool writer_finished_{false};
    bool started_ = false;
    bool joined_ = false;
    bool asset_transferred_ = false;
    float raw_peak_ = 0.0F;
};

} // namespace capture_panel::asio
