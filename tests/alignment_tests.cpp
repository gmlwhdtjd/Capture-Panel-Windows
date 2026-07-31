#include "test_framework.hpp"

#include "capture_panel/core/alignment.hpp"
#include "capture_panel/core/errors.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

using namespace capture_panel;

namespace {

[[nodiscard]] bool contains_warning(
    const std::vector<CaptureWarning>& warnings,
    CaptureWarning warning) {
    return std::find(warnings.begin(), warnings.end(), warning) != warnings.end();
}

[[nodiscard]] bool samples_near(
    const std::vector<float>& left,
    const std::vector<float>& right,
    double tolerance = 1.0e-6) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::abs(static_cast<double>(left[index] - right[index])) > tolerance) {
            return false;
        }
    }
    return true;
}

struct CancellingReaderState {
    std::shared_ptr<CancellationToken> cancellation;
    std::size_t read_call_count = 0;
};

class CancellingAlignmentReader final : public IFloat32FrameReader {
public:
    CancellingAlignmentReader(
        std::shared_ptr<CancellingReaderState> state,
        const std::int64_t start_frame,
        const std::int64_t frame_count)
        : state_(std::move(state)),
          cursor_(start_frame),
          frame_count_(frame_count) {}

    [[nodiscard]] std::uint32_t channel_count() const noexcept override {
        return 1;
    }

    std::int64_t read_frames(std::span<float> interleaved_samples) override {
        if (cursor_ >= frame_count_) return 0;
        const auto frames = std::min<std::int64_t>({
            frame_count_ - cursor_,
            static_cast<std::int64_t>(interleaved_samples.size()),
            128,
        });
        std::fill_n(
            interleaved_samples.data(),
            static_cast<std::size_t>(frames),
            0.0F);
        cursor_ += frames;
        ++state_->read_call_count;
        if (state_->read_call_count == 2) {
            static_cast<void>(state_->cancellation->cancel());
        }
        return frames;
    }

private:
    std::shared_ptr<CancellingReaderState> state_;
    std::int64_t cursor_ = 0;
    std::int64_t frame_count_ = 0;
};

} // namespace

CP_TEST_CASE("alignment playback plan preserves marker preamble and payload placement") {
    const AudioBuffer source{1'000.0, 1, {0.25F, -0.5F, 0.75F}};
    const auto plan = make_alignment_playback_plan(source);
    const auto audio = materialize_playback_plan(plan);

    CP_REQUIRE(plan.marker_frames == std::vector<std::int64_t>({0, 100, 200, 300, 400}));
    CP_REQUIRE(plan.source_start_frame == 2'400);
    CP_REQUIRE(plan.playback_frame_count == 2'403);
    CP_REQUIRE(audio.frame_count() == 2'403);
    const auto marker_amplitude = std::pow(10.0, -12.0 / 20.0);
    CP_REQUIRE_NEAR(audio.samples[0], marker_amplitude, 1.0e-6);
    CP_REQUIRE_NEAR(audio.samples[100], marker_amplitude, 1.0e-6);
    CP_REQUIRE_NEAR(audio.samples[400], marker_amplitude, 1.0e-6);
    CP_REQUIRE_NEAR(audio.samples[401], 0.0, 1.0e-9);
    CP_REQUIRE_NEAR(audio.samples[2'400], 0.25, 1.0e-6);
    CP_REQUIRE_NEAR(audio.samples[2'401], -0.5, 1.0e-6);
    CP_REQUIRE_NEAR(audio.samples[2'402], 0.75, 1.0e-6);
}

CP_TEST_CASE("alignment playback gain applies equally to markers and payload") {
    const AudioBuffer source{1'000.0, 1, {0.25F, -0.5F}};
    const auto gain = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
    const auto plan = make_alignment_playback_plan(source, gain);
    const auto audio = materialize_playback_plan(plan);

    CP_REQUIRE_NEAR(
        audio.samples[0],
        std::pow(10.0, -12.0 / 20.0) * gain,
        1.0e-6);
    CP_REQUIRE_NEAR(audio.samples[2'400], source.samples[0] * gain, 1.0e-6);
    CP_REQUIRE_NEAR(audio.samples[2'401], source.samples[1] * gain, 1.0e-6);
}

CP_TEST_CASE("marker matcher ignores extra candidates before the marker train") {
    const std::vector<std::int64_t> expected{400, 500, 600, 700, 800};
    std::vector<MarkerCandidate> candidates;
    for (const auto frame : {40, 140, 240, 340}) {
        candidates.push_back({frame, 0.8F});
    }
    for (const auto frame : expected) candidates.push_back({frame + 30, 0.6F});

    const auto result = select_marker_sequence(candidates, expected, 1'000.0);
    CP_REQUIRE(result.has_value());
    CP_REQUIRE(result->frames() == std::vector<std::int64_t>({430, 530, 630, 730, 830}));
}

CP_TEST_CASE("marker matcher penalizes negative latency and louder delayed echoes") {
    const std::vector<std::int64_t> expected{400, 500, 600, 700, 800};
    std::vector<MarkerCandidate> candidates;
    for (const auto frame : {200, 300, 400, 500, 600}) {
        candidates.push_back({frame, 0.9F});
    }
    for (const auto frame : expected) {
        candidates.push_back({frame + 30, 0.46F});
        candidates.push_back({frame + 80, 0.95F});
    }

    const auto result = select_marker_sequence(candidates, expected, 1'000.0);
    CP_REQUIRE(result.has_value());
    CP_REQUIRE(result->frames() == std::vector<std::int64_t>({430, 530, 630, 730, 830}));
}

CP_TEST_CASE("marker matcher preserves expected indices when the first marker is missing") {
    const std::vector<std::int64_t> expected{400, 500, 600, 700, 800};
    const std::vector<MarkerCandidate> candidates{
        {530, 0.6F}, {630, 0.6F}, {730, 0.6F}, {830, 0.6F},
    };

    const auto result = select_marker_sequence(candidates, expected, 1'000.0);
    CP_REQUIRE(result.has_value());
    CP_REQUIRE(result->frames() == std::vector<std::int64_t>({530, 630, 730, 830}));
    CP_REQUIRE(result->expected_indices == std::vector<std::size_t>({1, 2, 3, 4}));
}

CP_TEST_CASE("marker matcher preserves expected indices when a middle marker is missing") {
    const std::vector<std::int64_t> expected{400, 500, 600, 700, 800};
    const std::vector<MarkerCandidate> candidates{
        {430, 0.6F}, {530, 0.6F}, {730, 0.6F}, {830, 0.6F},
    };

    const auto result = select_marker_sequence(candidates, expected, 1'000.0);
    CP_REQUIRE(result.has_value());
    CP_REQUIRE(result->frames() == std::vector<std::int64_t>({430, 530, 730, 830}));
    CP_REQUIRE(result->expected_indices == std::vector<std::size_t>({0, 1, 3, 4}));
}

CP_TEST_CASE("marker matcher prefers an incomplete direct train over a complete delayed echo") {
    constexpr auto sample_rate = 48'000.0;
    constexpr std::int64_t direct_latency = 480;
    constexpr std::int64_t echo_delay = 2'400;
    const std::vector<std::int64_t> expected{24'000, 28'800, 33'600, 38'400, 43'200};

    for (const auto missing_index : {std::size_t{0}, std::size_t{2}}) {
        std::vector<MarkerCandidate> candidates;
        std::vector<std::int64_t> direct_frames;
        std::vector<std::size_t> direct_indices;
        for (std::size_t index = 0; index < expected.size(); ++index) {
            if (index != missing_index) {
                candidates.push_back({expected[index] + direct_latency, 0.6F});
                direct_frames.push_back(expected[index] + direct_latency);
                direct_indices.push_back(index);
            }
            candidates.push_back(
                {expected[index] + direct_latency + echo_delay, 0.95F});
        }

        const auto result = select_marker_sequence(candidates, expected, sample_rate);
        CP_REQUIRE(result.has_value());
        CP_REQUIRE(result->frames() == direct_frames);
        CP_REQUIRE(result->expected_indices == direct_indices);
    }
}

CP_TEST_CASE("marker matcher treats nearby timing hypotheses as one evidence band") {
    constexpr auto sample_rate = 1'000.0;
    constexpr std::int64_t incomplete_latency = 30;
    constexpr std::int64_t complete_latency = 50;
    const std::vector<std::int64_t> expected{400, 500, 600, 700, 800};
    std::vector<MarkerCandidate> candidates;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (index != 2) {
            candidates.push_back({expected[index] + incomplete_latency, 0.6F});
        }
        candidates.push_back({expected[index] + complete_latency, 0.5F});
    }

    const auto result = select_marker_sequence(candidates, expected, sample_rate);
    CP_REQUIRE(result.has_value());
    CP_REQUIRE(result->frames() == std::vector<std::int64_t>({450, 550, 650, 750, 850}));
    CP_REQUIRE(result->expected_indices == std::vector<std::size_t>({0, 1, 2, 3, 4}));
}

CP_TEST_CASE("payload alignment stays correct when the first or a middle marker is missing") {
    constexpr auto sample_rate = 1'000.0;
    constexpr std::int64_t latency = 30;
    const std::vector<std::int64_t> expected{400, 500, 600, 700, 800};
    constexpr std::int64_t source_start = 1'100;
    const std::vector<float> source{0.12F, -0.34F, 0.56F};

    for (const auto missing_index : {std::size_t{0}, std::size_t{2}}) {
        const auto trim_start = source_start + latency;
        std::vector<float> recorded(
            static_cast<std::size_t>(trim_start) + source.size() + 20U,
            0.0F);
        for (std::size_t index = 0; index < expected.size(); ++index) {
            if (index == missing_index) continue;
            recorded[static_cast<std::size_t>(expected[index] + latency)] = 0.6F;
        }
        std::copy(source.begin(), source.end(), recorded.begin() + trim_start);

        const auto result = align_payload(
            AudioBuffer{sample_rate, 1, recorded},
            AlignmentReference{expected, source_start, static_cast<std::int64_t>(source.size())});
        CP_REQUIRE(result.info.marker_latency_samples == latency);
        CP_REQUIRE(samples_near(result.payload.materialize().samples, source));
    }
}

CP_TEST_CASE("payload alignment rejects unrelated transient timing") {
    constexpr auto sample_rate = 1'000.0;
    const std::vector<std::int64_t> expected{400, 500, 600, 700, 800};
    constexpr std::int64_t source_start = 1'100;
    std::vector<float> recorded(1'300, 0.0F);
    for (const auto frame : {430, 575, 790}) {
        recorded[static_cast<std::size_t>(frame)] = 0.8F;
    }

    bool failed = false;
    try {
        static_cast<void>(align_payload(
            AudioBuffer{sample_rate, 1, recorded},
            AlignmentReference{expected, source_start, 3}));
    } catch (const CaptureError& error) {
        failed = error.code() == ErrorCode::alignment_failed;
    }
    CP_REQUIRE(failed);
}

CP_TEST_CASE("payload alignment uses source start plus median marker latency") {
    constexpr auto sample_rate = 1'000.0;
    constexpr std::int64_t latency = 12;
    const std::vector<std::int64_t> expected{100, 200, 350};
    constexpr std::int64_t source_start = 1'250;
    const std::vector<float> source{0.11F, 0.22F, 0.33F, 0.44F, 0.55F};
    const auto trim_start = source_start + latency;
    std::vector<float> recorded(
        static_cast<std::size_t>(trim_start) + source.size() + 100,
        0.0F);
    for (const auto marker : expected) {
        recorded[static_cast<std::size_t>(marker + latency)] = 1.0F;
    }
    std::copy(source.begin(), source.end(), recorded.begin() + trim_start);

    const auto result = align_payload(
        AudioBuffer{sample_rate, 1, recorded},
        AlignmentReference{expected, source_start, static_cast<std::int64_t>(source.size())});

    CP_REQUIRE(result.info.marker_latency_samples == latency);
    CP_REQUIRE(result.info.trim_start_frame == trim_start);
    CP_REQUIRE(result.info.trimmed_frame_count == static_cast<std::int64_t>(source.size()));
    CP_REQUIRE(samples_near(result.payload.materialize().samples, source));
    CP_REQUIRE(result.impulse_detection.has_value());
    CP_REQUIRE(result.impulse_detection->detected_positions.size() == expected.size());
    CP_REQUIRE(result.warnings.empty());
}

CP_TEST_CASE("payload alignment warns on low but sufficient marker evidence") {
    constexpr auto sample_rate = 1'000.0;
    constexpr std::int64_t latency = 30;
    const std::vector<std::int64_t> expected{400, 500, 600, 700, 800};
    constexpr std::int64_t source_start = 1'100;
    const std::vector<float> source{0.11F, 0.22F, 0.33F};
    const auto trim_start = source_start + latency;
    std::vector<float> recorded(
        static_cast<std::size_t>(trim_start) + source.size() + 100,
        0.0F);
    for (std::size_t index = 0; index < 3; ++index) {
        recorded[static_cast<std::size_t>(expected[index] + latency)] = 0.6F;
    }
    std::copy(source.begin(), source.end(), recorded.begin() + trim_start);

    const auto result = align_payload(
        AudioBuffer{sample_rate, 1, recorded},
        AlignmentReference{expected, source_start, static_cast<std::int64_t>(source.size())});

    CP_REQUIRE(result.info.marker_latency_samples == latency);
    CP_REQUIRE(result.impulse_detection->detected_positions.size() == 3);
    CP_REQUIRE(result.warnings.size() == 1);
    CP_REQUIRE(contains_warning(result.warnings, CaptureWarning::marker_evidence_low));
}

CP_TEST_CASE("payload alignment rejects marker evidence below the absolute threshold") {
    constexpr auto sample_rate = 1'000.0;
    constexpr std::int64_t latency = 30;
    const std::vector<std::int64_t> expected{100, 200, 300, 400, 500};
    constexpr std::int64_t source_start = 900;
    std::vector<float> recorded(1'100, 0.0F);
    for (const auto marker : expected) {
        recorded[static_cast<std::size_t>(marker + latency)] = 0.004F;
    }

    bool failed = false;
    try {
        static_cast<void>(align_payload(
            AudioBuffer{sample_rate, 1, recorded},
            AlignmentReference{expected, source_start, 4}));
    } catch (const CaptureError& error) {
        failed = error.code() == ErrorCode::alignment_failed;
    }
    CP_REQUIRE(failed);
}

CP_TEST_CASE("payload alignment rejects too few otherwise valid markers") {
    constexpr auto sample_rate = 1'000.0;
    constexpr std::int64_t latency = 30;
    const std::vector<std::int64_t> expected{100, 200, 300, 400, 500};
    constexpr std::int64_t source_start = 900;
    std::vector<float> recorded(1'100, 0.0F);
    recorded[static_cast<std::size_t>(expected[0] + latency)] = 0.6F;
    recorded[static_cast<std::size_t>(expected[1] + latency)] = 0.6F;

    bool failed = false;
    try {
        static_cast<void>(align_payload(
            AudioBuffer{sample_rate, 1, recorded},
            AlignmentReference{expected, source_start, 4}));
    } catch (const CaptureError& error) {
        failed = error.code() == ErrorCode::alignment_failed;
    }
    CP_REQUIRE(failed);
}

CP_TEST_CASE("payload transients after the marker search boundary are not alignment evidence") {
    constexpr auto sample_rate = 1'000.0;
    const std::vector<std::int64_t> expected{100, 200, 300, 400, 500};
    constexpr std::int64_t source_start = 5'500;
    std::vector<float> recorded(6'300, 0.0F);
    for (std::int64_t offset = 0; offset <= 400; offset += 100) {
        recorded[static_cast<std::size_t>(source_start + offset)] = 0.8F;
    }

    bool failed = false;
    try {
        static_cast<void>(align_payload(
            AudioBuffer{sample_rate, 1, recorded},
            AlignmentReference{expected, source_start, 700}));
    } catch (const CaptureError& error) {
        failed = error.code() == ErrorCode::alignment_failed;
    }
    CP_REQUIRE(failed);
}

CP_TEST_CASE("payload alignment zero pads when an aligned recording ends early") {
    constexpr auto sample_rate = 1'000.0;
    constexpr std::int64_t latency = 10;
    const std::vector<std::int64_t> expected{50, 150, 250};
    constexpr std::int64_t source_start = 350;
    constexpr std::int64_t target_frames = 6;
    const auto trim_start = source_start + latency;
    std::vector<float> recorded(static_cast<std::size_t>(trim_start + 3), 0.0F);
    for (const auto marker : expected) {
        recorded[static_cast<std::size_t>(marker + latency)] = 1.0F;
    }
    recorded[static_cast<std::size_t>(trim_start)] = 0.1F;
    recorded[static_cast<std::size_t>(trim_start + 1)] = 0.2F;
    recorded[static_cast<std::size_t>(trim_start + 2)] = 0.3F;

    const auto result = align_payload(
        AudioBuffer{sample_rate, 1, recorded},
        AlignmentReference{expected, source_start, target_frames});

    CP_REQUIRE(result.info.trim_start_frame == trim_start);
    CP_REQUIRE(result.info.trimmed_frame_count == 3);
    CP_REQUIRE(samples_near(
        result.payload.materialize().samples,
        std::vector<float>{0.1F, 0.2F, 0.3F, 0.0F, 0.0F, 0.0F}));
}

CP_TEST_CASE("payload alignment observes cancellation between reader chunks") {
    constexpr std::int64_t frame_count = 1'100;
    const auto cancellation = std::make_shared<CancellationToken>();
    const auto state = std::make_shared<CancellingReaderState>(
        CancellingReaderState{.cancellation = cancellation});
    const auto recorded = Float32AudioAsset::from_reader_factory(
        1'000.0,
        1,
        frame_count,
        [state, frame_count](const std::int64_t start_frame) {
            return std::make_unique<CancellingAlignmentReader>(
                state, start_frame, frame_count);
        });

    bool cancelled = false;
    try {
        static_cast<void>(align_payload(
            recorded,
            1.0F,
            AlignmentReference{
                .expected_marker_frames = {100, 200, 300, 400, 500},
                .source_start_frame = 900,
                .source_frame_count = 4,
            },
            cancellation));
    } catch (const CaptureError& error) {
        cancelled = error.code() == ErrorCode::capture_cancelled;
    }

    CP_REQUIRE(cancelled);
    CP_REQUIRE(cancellation->is_cancelled());
    CP_REQUIRE(state->read_call_count == 2);
}
