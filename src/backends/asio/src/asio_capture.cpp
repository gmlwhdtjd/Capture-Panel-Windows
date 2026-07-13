#include "capture_panel/asio/asio_backend.hpp"

#include "asio_backend_helpers.hpp"
#include "asio_buffer_timeline.hpp"
#include "asio_callback_lifetime.hpp"
#include "asio_driver_session.hpp"
#include "asio_sample_converter.hpp"
#include "asio_session_gate.hpp"
#include "asio_stream_workers.hpp"
#include "capture_panel/core/channels.hpp"
#include "capture_panel/core/errors.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>

#include "iasiodrv.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace capture_panel::asio {
namespace {

[[nodiscard]] std::size_t checked_sample_count(
    const std::int64_t frames,
    const std::size_t channels,
    const std::string_view purpose) {
    if (frames < 0 || channels == 0) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "Invalid " + std::string(purpose) + " audio buffer dimensions.");
    }
    const auto frame_count = static_cast<std::uint64_t>(frames);
    if (frame_count > std::numeric_limits<std::size_t>::max() / channels) {
        throw CaptureError(
            ErrorCode::validation_failed,
            std::string(purpose) + " audio buffer is too large.");
    }
    return static_cast<std::size_t>(frame_count) * channels;
}

[[nodiscard]] std::int64_t padding_frames(const RawAudioCaptureRequest& request) {
    if (!std::isfinite(request.padding_seconds) || request.padding_seconds < 0.0) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "ASIO capture padding must be a non-negative finite value.");
    }
    const auto frames = request.padding_seconds * request.playback_plan.sample_rate();
    if (!std::isfinite(frames)
        || frames >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        throw CaptureError(ErrorCode::validation_failed, "ASIO capture padding is too large.");
    }
    return static_cast<std::int64_t>(std::llround(frames));
}

void validate_playback_plan(const CapturePassPlaybackPlan& playback) {
    if (!playback.source.valid() || !std::isfinite(playback.sample_rate())
        || playback.sample_rate() <= 0.0) {
        throw CaptureError(
            ErrorCode::unsupported_sample_rate,
            "ASIO playback sample rate must be a positive finite value.");
    }
    if (playback.channel_count() == 0 || playback.playback_frame_count <= 0
        || playback.source.format().total_frames <= 0) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "ASIO playback audio must contain at least one frame and channel.");
    }
}

void reject_duplicate_channels(
    const std::vector<std::uint32_t>& channels,
    const std::string_view direction) {
    std::unordered_set<std::uint32_t> seen;
    seen.reserve(channels.size());
    for (const auto channel : channels) {
        if (!seen.insert(channel).second) {
            throw CaptureError(
                ErrorCode::invalid_channel_specification,
                "ASIO " + std::string(direction)
                    + " channels cannot contain duplicates (channel "
                    + std::to_string(channel) + ").");
        }
    }
}

void configure_sample_rate(IASIO& driver, const double requested) {
    ASIOSampleRate current = 0.0;
    if (asio_result_succeeded(driver.getSampleRate(&current))
        && std::isfinite(current)
        && std::abs(current - requested) <= 0.5) {
        return;
    }
    require_asio_result(
        driver,
        driver.canSampleRate(requested),
        "select the playback sample rate",
        ErrorCode::unsupported_sample_rate);
    require_asio_result(
        driver,
        driver.setSampleRate(requested),
        "set the playback sample rate",
        ErrorCode::unsupported_sample_rate);

    current = 0.0;
    require_asio_result(
        driver,
        driver.getSampleRate(&current),
        "verify the ASIO sample rate",
        ErrorCode::unsupported_sample_rate);
    if (!std::isfinite(current) || std::abs(current - requested) > 0.5) {
        throw CaptureError(
            ErrorCode::unsupported_sample_rate,
            "The ASIO driver did not apply the requested sample rate.");
    }
}

[[nodiscard]] long preferred_buffer_size(IASIO& driver) {
    long minimum = 0;
    long maximum = 0;
    long preferred = 0;
    long granularity = 0;
    require_asio_result(
        driver,
        driver.getBufferSize(&minimum, &maximum, &preferred, &granularity),
        "query the ASIO buffer size");
    static_cast<void>(granularity);
    if (minimum <= 0 || maximum < minimum || preferred < minimum || preferred > maximum) {
        throw CaptureError(
            ErrorCode::backend_failure,
            "The ASIO driver returned invalid buffer-size constraints.");
    }
    return preferred;
}

[[nodiscard]] std::vector<ASIOChannelInfo> query_channel_info(
    IASIO& driver,
    const std::vector<std::uint32_t>& one_based_channels,
    const bool is_input) {
    std::vector<ASIOChannelInfo> result;
    result.reserve(one_based_channels.size());
    for (const auto one_based : one_based_channels) {
        ASIOChannelInfo info{};
        info.channel = static_cast<long>(one_based - 1U);
        info.isInput = is_input ? ASIOTrue : ASIOFalse;
        require_asio_result(
            driver,
            driver.getChannelInfo(&info),
            is_input ? "query a selected ASIO input" : "query a selected ASIO output");
        if (!is_supported_asio_sample_type(info.type)) {
            throw CaptureError(
                ErrorCode::unsupported_format,
                "ASIO channel " + std::to_string(one_based) + " uses unsupported sample type "
                    + std::to_string(info.type) + ".");
        }
        result.push_back(info);
    }
    return result;
}

struct CaptureState final {
    IASIO* driver = nullptr;
    AsioStreamWorkers* streams = nullptr;
    std::vector<ASIOBufferInfo>* buffer_infos = nullptr;
    const std::vector<ASIOChannelInfo>* output_info = nullptr;
    const std::vector<ASIOChannelInfo>* input_info = nullptr;
    std::int64_t padding_frame_count = 0;
    std::int64_t playback_frame_count = 0;
    std::int64_t playback_frames_consumed = 0;
    std::size_t playback_channel_count = 0;
    std::size_t buffer_frame_count = 0;
    std::vector<float> playback_scratch;
    std::vector<float> output_scratch;
    std::vector<float> input_scratch;
    std::vector<MutableAsioChannelBuffer> output_views;
    std::vector<ConstAsioChannelBuffer> input_views;
    std::vector<std::size_t> output_byte_counts;
    std::vector<std::size_t> input_byte_counts;
    AsioBufferTimeline timeline;
    bool output_ready_supported = false;
    std::atomic<std::int64_t> completed_frames{0};
    std::atomic_bool done{false};
    std::atomic_bool conversion_failed{false};
    std::atomic_bool invalid_buffer_index{false};
    std::atomic_bool reentrant_callback{false};
    std::atomic_bool reset_requested{false};
    std::atomic_bool resync_requested{false};
    std::atomic_bool latencies_changed{false};
    std::atomic_bool sample_rate_changed{false};
    std::atomic_bool overload{false};
    std::atomic_flag processing = ATOMIC_FLAG_INIT;

    CaptureState(
        IASIO& asio_driver,
        const CapturePassPlaybackPlan& playback,
        AsioStreamWorkers& stream_workers,
        std::vector<ASIOBufferInfo>& buffers,
        const std::vector<ASIOChannelInfo>& outputs,
        const std::vector<ASIOChannelInfo>& inputs,
        const std::int64_t padding_frames_value,
        const std::int64_t total_frames_value,
        const long asio_buffer_size)
        : driver(&asio_driver),
          streams(&stream_workers),
          buffer_infos(&buffers),
          output_info(&outputs),
          input_info(&inputs),
          padding_frame_count(padding_frames_value),
          playback_frame_count(playback.playback_frame_count),
          playback_channel_count(playback.channel_count()),
          buffer_frame_count(static_cast<std::size_t>(asio_buffer_size)),
          playback_scratch(
              checked_sample_count(
                  asio_buffer_size,
                  playback.channel_count(),
                  "ASIO callback source"),
              0.0F),
          output_scratch(
              checked_sample_count(
                  asio_buffer_size,
                  outputs.size(),
                  "ASIO callback playback"),
              0.0F),
          input_scratch(
              checked_sample_count(
                  asio_buffer_size,
                  inputs.size(),
                  "ASIO callback recording"),
              0.0F),
          output_views(outputs.size()),
          input_views(inputs.size()),
          output_byte_counts(outputs.size()),
          input_byte_counts(inputs.size()),
          timeline(
              total_frames_value,
              static_cast<std::size_t>(asio_buffer_size)) {
        for (std::size_t index = 0; index < outputs.size(); ++index) {
            output_byte_counts[index] = buffer_frame_count * asio_sample_size(outputs[index].type);
            output_views[index].sample_type = outputs[index].type;
        }
        for (std::size_t index = 0; index < inputs.size(); ++index) {
            input_byte_counts[index] = buffer_frame_count * asio_sample_size(inputs[index].type);
            input_views[index].sample_type = inputs[index].type;
        }
    }

    [[nodiscard]] bool write_output_buffer(
        const std::size_t half,
        const AsioFrameBlock block) noexcept {
        const auto output_channels = output_info->size();

        std::fill(output_scratch.begin(), output_scratch.end(), 0.0F);
        const auto block_end = block.start_frame + static_cast<std::int64_t>(block.frame_count);
        const auto playback_start = padding_frame_count;
        const auto playback_end = playback_start + playback_frame_count;
        const auto overlap_start = std::max(block.start_frame, playback_start);
        const auto overlap_end = std::min(block_end, playback_end);
        if (overlap_end > overlap_start) {
            const auto overlap_frames = static_cast<std::size_t>(overlap_end - overlap_start);
            const auto source_samples = overlap_frames * playback_channel_count;
            if (!streams->playback_ring().read_exact(
                    std::span<float>(playback_scratch).first(source_samples))) {
                streams->fail_playback_underflow();
            } else {
                const auto destination_frame = static_cast<std::size_t>(
                    overlap_start - block.start_frame);
                const auto mapped_channels = std::min(
                    output_channels,
                    playback_channel_count);
                for (std::size_t frame = 0; frame < overlap_frames; ++frame) {
                    for (std::size_t channel = 0; channel < mapped_channels; ++channel) {
                        output_scratch[(destination_frame + frame) * output_channels + channel]
                            = playback_scratch[frame * playback_channel_count + channel];
                    }
                }
                playback_frames_consumed += static_cast<std::int64_t>(overlap_frames);
                if (playback_frames_consumed >= playback_frame_count) {
                    streams->mark_playback_consumed();
                }
            }
        }

        for (std::size_t channel = 0; channel < output_channels; ++channel) {
            auto* pointer = (*buffer_infos)[channel].buffers[half];
            if (pointer == nullptr) return false;
            output_views[channel].bytes = {
                static_cast<std::byte*>(pointer),
                output_byte_counts[channel],
            };
        }

        return interleaved_float_to_asio(
            output_scratch,
            buffer_frame_count,
            output_views) == SampleConversionResult::success;
    }

    [[nodiscard]] bool read_input_buffer(
        const std::size_t half,
        const std::int64_t start_frame,
        const std::size_t frames) noexcept {
        const auto output_channels = output_info->size();
        const auto input_channels = input_info->size();
        for (std::size_t channel = 0; channel < input_channels; ++channel) {
            auto* pointer = (*buffer_infos)[output_channels + channel].buffers[half];
            if (pointer == nullptr) return false;
            input_views[channel].bytes = {
                static_cast<const std::byte*>(pointer),
                input_byte_counts[channel],
            };
        }

        static_cast<void>(start_frame);
        const auto destination_size = frames * input_channels;
        const auto converted = asio_to_interleaved_float(
            input_views,
            frames,
            std::span<float>(input_scratch).first(destination_size))
            == SampleConversionResult::success;
        if (!converted) return false;
        if (!streams->recording_ring().write_exact(
                std::span<const float>(input_scratch).first(destination_size))) {
            streams->fail_recording_overflow();
        }
        return true;
    }

    [[nodiscard]] bool prime_output() noexcept {
        return write_output_buffer(1, timeline.next_output_block());
    }

    void process(const long double_buffer_index) noexcept {
        if (processing.test_and_set(std::memory_order_acquire)) {
            reentrant_callback.store(true, std::memory_order_release);
            return;
        }

        if (double_buffer_index < 0 || double_buffer_index > 1) {
            invalid_buffer_index.store(true, std::memory_order_release);
            processing.clear(std::memory_order_release);
            done.store(true, std::memory_order_release);
            return;
        }
        const auto half = static_cast<std::size_t>(double_buffer_index);
        auto conversion_ok = write_output_buffer(
            half,
            timeline.next_output_block());

        if (conversion_ok) {
            const auto input_block = timeline.next_input_block();
            if (input_block.has_value() && input_block->frame_count > 0) {
                conversion_ok = read_input_buffer(
                    half,
                    input_block->start_frame,
                    input_block->frame_count);
                if (conversion_ok) {
                    completed_frames.store(
                        timeline.recorded_frame_count(),
                        std::memory_order_release);
                }
            }
        }

        auto should_finish = false;
        if (!conversion_ok) {
            conversion_failed.store(true, std::memory_order_release);
            should_finish = true;
        } else if (streams->failed()) {
            should_finish = true;
        } else if (timeline.recording_complete()) {
            if (!streams->playback_consumed()) {
                streams->fail_playback_underflow();
            }
            streams->mark_recording_producer_finished();
            should_finish = true;
        }
        should_finish = should_finish
            || reentrant_callback.load(std::memory_order_acquire);

        if (output_ready_supported) {
            static_cast<void>(driver->outputReady());
        }
        processing.clear(std::memory_order_release);
        if (should_finish) done.store(true, std::memory_order_release);
    }
};

static_assert(std::atomic<std::int64_t>::is_always_lock_free);
static_assert(std::atomic_bool::is_always_lock_free);
static_assert(std::atomic<AsioStreamFailure>::is_always_lock_free);

CallbackLifetimeGate<CaptureState> active_capture;

void buffer_switch(const long double_buffer_index, const ASIOBool direct_process) noexcept {
    static_cast<void>(direct_process);
    if (auto reader = active_capture.try_acquire(); reader.has_value()) {
        reader->get().process(double_buffer_index);
    }
}

void sample_rate_did_change(const ASIOSampleRate sample_rate) noexcept {
    static_cast<void>(sample_rate);
    if (auto reader = active_capture.try_acquire(); reader.has_value()) {
        reader->get().sample_rate_changed.store(true, std::memory_order_release);
    }
}

[[nodiscard]] bool supports_asio_selector(const long selector) noexcept {
    switch (selector) {
    case kAsioEngineVersion:
    case kAsioResetRequest:
    case kAsioResyncRequest:
    case kAsioLatenciesChanged:
    case kAsioSupportsTimeInfo:
    case kAsioOverload:
        return true;
    default:
        return false;
    }
}

long asio_message(
    const long selector,
    const long value,
    void* message,
    double* optional) noexcept {
    static_cast<void>(message);
    static_cast<void>(optional);
    if (selector == kAsioSelectorSupported) {
        return supports_asio_selector(value) ? 1L : 0L;
    }
    if (selector == kAsioEngineVersion) return 2L;
    if (selector == kAsioSupportsTimeInfo) return 1L;

    auto reader = active_capture.try_acquire();
    if (!reader.has_value()) return 0L;
    auto& state = reader->get();
    switch (selector) {
    case kAsioResetRequest:
        state.reset_requested.store(true, std::memory_order_release);
        return 1L;
    case kAsioResyncRequest:
        state.resync_requested.store(true, std::memory_order_release);
        return 1L;
    case kAsioLatenciesChanged:
        state.latencies_changed.store(true, std::memory_order_release);
        return 1L;
    case kAsioOverload:
        state.overload.store(true, std::memory_order_release);
        return 1L;
    default:
        return 0L;
    }
}

ASIOTime* buffer_switch_time_info(
    ASIOTime* parameters,
    const long double_buffer_index,
    const ASIOBool direct_process) noexcept {
    buffer_switch(double_buffer_index, direct_process);
    return parameters;
}

class ActiveCaptureGuard final {
public:
    explicit ActiveCaptureGuard(CaptureState& state) : state_(&state) {
        if (!active_capture.try_activate(state)) {
            throw CaptureError(
                ErrorCode::backend_failure,
                "Only one ASIO capture can run in this process at a time.");
        }
    }

    ~ActiveCaptureGuard() {
        deactivate_and_wait();
    }

    ActiveCaptureGuard(const ActiveCaptureGuard&) = delete;
    ActiveCaptureGuard& operator=(const ActiveCaptureGuard&) = delete;

    void deactivate_and_wait() noexcept {
        if (state_ == nullptr) return;
        active_capture.deactivate_and_wait();
        state_ = nullptr;
    }

private:
    CaptureState* state_ = nullptr;
};

struct CaptureCleanupResults {
    long stop_result = ASE_OK;
    long dispose_result = ASE_OK;
};

class CaptureResourceGuard final {
public:
    CaptureResourceGuard(
        IASIO& driver,
        CaptureState& state,
        AsioStreamWorkers& stream_workers)
        : driver_(&driver),
          stream_workers_(&stream_workers),
          active_capture_(state) {}
    ~CaptureResourceGuard() { static_cast<void>(cleanup()); }

    CaptureResourceGuard(const CaptureResourceGuard&) = delete;
    CaptureResourceGuard& operator=(const CaptureResourceGuard&) = delete;

    // Arm before invoking the corresponding driver operation. Some drivers
    // partially allocate/start before returning an error.
    void arm_buffers() noexcept { buffers_may_exist_ = true; }
    void arm_stream() noexcept { stream_may_be_running_ = true; }

    [[nodiscard]] CaptureCleanupResults cleanup(
        const bool drain_completed_recording = false) noexcept {
        if (cleaned_) return results_;
        cleaned_ = true;

        // ASIOStop guarantees that no new callbacks are issued after it
        // returns. Closing the gate next rejects a non-compliant late callback
        // and waits for every callback that entered before close. Buffers are
        // disposed only after that lifetime barrier.
        if (stream_may_be_running_) {
            stream_may_be_running_ = false;
            results_.stop_result = driver_->stop();
        }
        active_capture_.deactivate_and_wait();

        // With callbacks stopped and drained, no producer/consumer can touch
        // either ring again. A complete capture lets the writer drain its last
        // exact block and flush/close; every abnormal path wakes both workers
        // before joining so a full/empty ring cannot deadlock cleanup.
        if (!drain_completed_recording) stream_workers_->request_stop();
        stream_workers_->join();
        if (buffers_may_exist_) {
            buffers_may_exist_ = false;
            results_.dispose_result = driver_->disposeBuffers();
        }
        return results_;
    }

private:
    IASIO* driver_ = nullptr;
    AsioStreamWorkers* stream_workers_ = nullptr;
    ActiveCaptureGuard active_capture_;
    bool buffers_may_exist_ = false;
    bool stream_may_be_running_ = false;
    bool cleaned_ = false;
    CaptureCleanupResults results_;
};

[[nodiscard]] bool fatal_driver_event(const CaptureState& state) noexcept {
    return state.streams->failed()
        || state.reset_requested.load(std::memory_order_acquire)
        || state.resync_requested.load(std::memory_order_acquire)
        || state.sample_rate_changed.load(std::memory_order_acquire)
        || state.overload.load(std::memory_order_acquire)
        || state.conversion_failed.load(std::memory_order_acquire)
        || state.invalid_buffer_index.load(std::memory_order_acquire)
        || state.reentrant_callback.load(std::memory_order_acquire);
}

[[noreturn]] void throw_capture_state_error(const CaptureState& state) {
    switch (state.streams->failure()) {
    case AsioStreamFailure::playback_underflow:
        throw CaptureError(
            ErrorCode::playback_stream_underflow,
            "The source audio could not be streamed to the ASIO driver fast enough.");
    case AsioStreamFailure::recording_overflow:
        throw CaptureError(
            ErrorCode::recording_stream_overflow,
            "The ASIO recording could not be drained to disk fast enough.");
    case AsioStreamFailure::source_failure:
        throw CaptureError(
            ErrorCode::source_stream_failure,
            "The source audio could not be read during ASIO capture.");
    case AsioStreamFailure::writer_failure:
        throw CaptureError(
            ErrorCode::recording_write_failure,
            "The temporary ASIO recording could not be written or closed.");
    case AsioStreamFailure::none:
        break;
    }
    if (state.reset_requested.load(std::memory_order_acquire)) {
        throw CaptureError(ErrorCode::backend_failure, "The ASIO driver requested a reset.");
    }
    if (state.resync_requested.load(std::memory_order_acquire)) {
        throw CaptureError(ErrorCode::backend_failure, "The ASIO driver lost synchronization.");
    }
    if (state.sample_rate_changed.load(std::memory_order_acquire)) {
        throw CaptureError(
            ErrorCode::backend_failure,
            "The ASIO sample rate changed during capture.");
    }
    if (state.overload.load(std::memory_order_acquire)) {
        throw CaptureError(
            ErrorCode::backend_failure,
            "The ASIO driver reported an audio-buffer overload.");
    }
    if (state.invalid_buffer_index.load(std::memory_order_acquire)) {
        throw CaptureError(
            ErrorCode::backend_failure,
            "The ASIO driver supplied an invalid double-buffer index.");
    }
    if (state.reentrant_callback.load(std::memory_order_acquire)) {
        throw CaptureError(
            ErrorCode::backend_failure,
            "The ASIO driver re-entered the audio callback.");
    }
    throw CaptureError(
        ErrorCode::backend_failure,
        "An ASIO sample conversion failed inside the audio callback.");
}

} // namespace

RawAudioCaptureResult AsioAudioBackend::capture(const RawAudioCaptureRequest& request) {
    const auto session_lease = acquire_asio_session();
    validate_playback_plan(request.playback_plan);
    if (request.cancellation && request.cancellation->is_cancelled()) {
        throw CaptureError(ErrorCode::capture_cancelled, "ASIO capture was cancelled.");
    }

    const auto registration = find_asio_driver(request.route.driver_id);
    AsioDriverSession session(registration);
    auto& driver = session.driver();
    configure_sample_rate(driver, request.playback_plan.sample_rate());

    long native_inputs = 0;
    long native_outputs = 0;
    require_asio_result(
        driver,
        driver.getChannels(&native_inputs, &native_outputs),
        "query ASIO channels for capture");
    if (native_inputs < 0 || native_outputs < 0) {
        throw CaptureError(
            ErrorCode::backend_failure,
            "The ASIO driver returned an invalid channel count.");
    }
    validate_playback_channels(
        request.route.playback_channels,
        static_cast<std::uint32_t>(native_outputs));
    validate_record_channels(
        request.route.record_channels,
        static_cast<std::uint32_t>(native_inputs));
    reject_duplicate_channels(request.route.playback_channels, "playback");
    reject_duplicate_channels(request.route.record_channels, "record");

    const auto output_info = query_channel_info(
        driver,
        request.route.playback_channels,
        false);
    const auto input_info = query_channel_info(
        driver,
        request.route.record_channels,
        true);
    const auto buffer_size = preferred_buffer_size(driver);

    const auto pre_pad_frames = padding_frames(request);
    const auto playback_frames = request.playback_plan.playback_frame_count;
    if (pre_pad_frames > (std::numeric_limits<std::int64_t>::max() - playback_frames) / 2) {
        throw CaptureError(ErrorCode::validation_failed, "ASIO capture duration is too large.");
    }
    const auto total_frames = pre_pad_frames + playback_frames + pre_pad_frames;
    const auto record_channels = request.route.record_channels.size();
    AsioStreamWorkers streams(
        request.playback_plan,
        static_cast<std::uint32_t>(record_channels),
        total_frames,
        static_cast<std::size_t>(buffer_size),
        request.scratch_file_prefix);
    streams.start();
    const auto buffer_frames = static_cast<std::size_t>(buffer_size);
    if (buffer_frames > std::numeric_limits<std::size_t>::max() / 4) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "The ASIO playback prebuffer is too large.");
    }
    const auto quarter_second = std::ceil(request.playback_plan.sample_rate() * 0.25);
    if (!std::isfinite(quarter_second)
        || quarter_second > static_cast<double>(std::numeric_limits<std::size_t>::max())) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "The ASIO playback prebuffer duration is too large.");
    }
    streams.wait_until_playback_ready(
        std::max(buffer_frames * 4, static_cast<std::size_t>(quarter_second)),
        std::chrono::seconds(10),
        request.cancellation);

    std::vector<ASIOBufferInfo> buffers;
    buffers.reserve(output_info.size() + input_info.size());
    for (const auto channel : request.route.playback_channels) {
        buffers.push_back({
            .isInput = ASIOFalse,
            .channelNum = static_cast<long>(channel - 1U),
            .buffers = {nullptr, nullptr},
        });
    }
    for (const auto channel : request.route.record_channels) {
        buffers.push_back({
            .isInput = ASIOTrue,
            .channelNum = static_cast<long>(channel - 1U),
            .buffers = {nullptr, nullptr},
        });
    }
    if (buffers.size() > static_cast<std::size_t>(std::numeric_limits<long>::max())) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "Too many ASIO channels were selected for one capture.");
    }

    CaptureState state(
        driver,
        request.playback_plan,
        streams,
        buffers,
        output_info,
        input_info,
        pre_pad_frames,
        total_frames,
        buffer_size);
    ASIOCallbacks callbacks{
        .bufferSwitch = buffer_switch,
        .sampleRateDidChange = sample_rate_did_change,
        .asioMessage = asio_message,
        .bufferSwitchTimeInfo = buffer_switch_time_info,
    };

    bool cancelled = false;
    bool timed_out = false;
    CaptureCleanupResults cleanup_results;
    {
        CaptureResourceGuard resources(driver, state, streams);
        resources.arm_buffers();
        require_asio_result(
            driver,
            driver.createBuffers(
                buffers.data(),
                static_cast<long>(buffers.size()),
                buffer_size,
                &callbacks),
            "create ASIO buffers");

        for (std::size_t channel = 0; channel < output_info.size(); ++channel) {
            const auto byte_count = static_cast<std::size_t>(buffer_size)
                * asio_sample_size(output_info[channel].type);
            for (std::size_t half = 0; half < 2; ++half) {
                if (buffers[channel].buffers[half] == nullptr) {
                    throw CaptureError(
                        ErrorCode::backend_failure,
                        "The ASIO driver returned a null output buffer.");
                }
                std::memset(buffers[channel].buffers[half], 0, byte_count);
            }
        }
        if (!state.prime_output()) {
            throw CaptureError(
                ErrorCode::backend_failure,
                "Could not prefill the first ASIO output buffer.");
        }
        if (streams.failed()) throw_capture_state_error(state);
        state.output_ready_supported = asio_result_succeeded(driver.outputReady());

        if (request.progress) request.progress(0, total_frames);
        if (request.cancellation && request.cancellation->is_cancelled()) {
            throw CaptureError(ErrorCode::capture_cancelled, "ASIO capture was cancelled.");
        }

        resources.arm_stream();
        require_asio_result(driver, driver.start(), "start ASIO capture");

        const auto expected_seconds = static_cast<double>(total_frames)
            / request.playback_plan.sample_rate();
        const auto driver_pipeline_seconds = 2.0 * static_cast<double>(buffer_size)
            / request.playback_plan.sample_rate();
        const auto timeout_seconds = std::max(
            10.0,
            expected_seconds + std::max(5.0, driver_pipeline_seconds));
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(timeout_seconds));
        std::int64_t reported_progress = 0;

        while (true) {
            session.pump_messages();
            if (request.cancellation && request.cancellation->is_cancelled()) {
                cancelled = true;
                break;
            }
            if (fatal_driver_event(state)) break;
            if (state.done.load(std::memory_order_acquire)) break;
            if (state.latencies_changed.exchange(false, std::memory_order_acq_rel)) {
                long input_latency = 0;
                long output_latency = 0;
                require_asio_result(
                    driver,
                    driver.getLatencies(&input_latency, &output_latency),
                    "refresh changed ASIO latencies");
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                timed_out = true;
                break;
            }

            const auto progress = state.completed_frames.load(std::memory_order_acquire);
            if (request.progress && progress != reported_progress) {
                request.progress(progress, total_frames);
                reported_progress = progress;
            }
            const auto wait_result = MsgWaitForMultipleObjects(
                0,
                nullptr,
                FALSE,
                10,
                QS_ALLINPUT);
            if (wait_result == WAIT_FAILED) Sleep(1);
        }

        const auto drain_completed_recording = !cancelled
            && !timed_out
            && !fatal_driver_event(state)
            && state.done.load(std::memory_order_acquire)
            && streams.recording_producer_finished();
        cleanup_results = resources.cleanup(drain_completed_recording);
    }

    // Cancellation wins a close race with a driver/worker failure, matching
    // the public capture contract.
    cancelled = cancelled
        || (request.cancellation && request.cancellation->is_cancelled());
    if (cancelled) {
        throw CaptureError(ErrorCode::capture_cancelled, "ASIO capture was cancelled.");
    }
    if (timed_out) {
        throw CaptureError(ErrorCode::capture_timed_out, "ASIO capture timed out.");
    }
    if (fatal_driver_event(state)) throw_capture_state_error(state);
    require_asio_result(driver, cleanup_results.stop_result, "stop ASIO capture");
    require_asio_result(driver, cleanup_results.dispose_result, "dispose ASIO buffers");

    const auto completed = state.completed_frames.load(std::memory_order_acquire);
    if (completed < total_frames) {
        throw CaptureError(
            ErrorCode::backend_failure,
            "ASIO capture stopped before all requested frames were recorded.");
    }
    if (!streams.writer_finished()) {
        throw CaptureError(
            ErrorCode::recording_write_failure,
            "The temporary ASIO recording did not finish flushing to disk.");
    }
    if (request.progress) request.progress(total_frames, total_frames);
    return {
        .recorded = streams.take_recorded_asset(),
        .pre_pad_frames = pre_pad_frames,
    };
}

} // namespace capture_panel::asio
