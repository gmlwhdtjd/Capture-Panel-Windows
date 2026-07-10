#include "test_framework.hpp"

#include "capture_panel/core/verification.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace capture_panel;

namespace {

[[nodiscard]] PayloadAlignmentInfo empty_alignment_info() {
    return PayloadAlignmentInfo{};
}

[[nodiscard]] bool contains_warning(
    const AlignmentVerificationResult& result,
    CaptureWarning warning) {
    return std::find(result.warnings.begin(), result.warnings.end(), warning)
        != result.warnings.end();
}

[[nodiscard]] bool contains_failure(
    const AlignmentVerificationResult& result,
    CaptureFailure failure) {
    return std::find(result.failures.begin(), result.failures.end(), failure)
        != result.failures.end();
}

} // namespace

CP_TEST_CASE("verification signal is a framed logarithmic sweep") {
    const auto signal = make_verification_signal(10'000.0, 1, -12.0);

    CP_REQUIRE(signal.leading_silence_frame_range.lower_bound == 0);
    CP_REQUIRE(signal.leading_silence_frame_range.upper_bound == 1'000);
    CP_REQUIRE(signal.sweep_frame_range.lower_bound == 1'000);
    CP_REQUIRE(signal.sweep_frame_range.upper_bound == 4'000);
    CP_REQUIRE(signal.format.total_frames == 9'000);
    CP_REQUIRE(signal.audio.frame_count() == 9'000);
    CP_REQUIRE_NEAR(signal.audio.samples[999], 0.0, 1.0e-9);
    const auto peak_sample = *std::max_element(
        signal.audio.samples.begin(), signal.audio.samples.end(),
        [](float left, float right) { return std::abs(left) < std::abs(right); });
    CP_REQUIRE(std::abs(peak_sample) > 0.2F);
}

CP_TEST_CASE("verification accepts a sweep at the expected aligned timing") {
    const auto signal = make_verification_signal(10'000.0, 1);
    const auto result = evaluate_verification(
        signal.audio, signal, empty_alignment_info(), -12.0);

    CP_REQUIRE(result.failures.empty());
    CP_REQUIRE(result.start_offset_frames == 0);
    CP_REQUIRE(result.timing_fit_error_frames == 0.0);
    CP_REQUIRE(result.max_timing_error_frames == 0);
    CP_REQUIRE(result.ambiguous_match_count == 0);
    CP_REQUIRE(result.sweep.has_value());
    CP_REQUIRE(result.sweep->detected_frame == signal.sweep_frame_range.lower_bound);
    CP_REQUIRE(result.sweep->error_frames == 0);
    CP_REQUIRE(result.sweep->reliability == VerificationReliability::reliable);
    CP_REQUIRE(result.sweep->direct_score > 0.99);
    CP_REQUIRE(result.warnings.empty());
}

CP_TEST_CASE("verification reports equipment decay in the leading silence") {
    const auto signal = make_verification_signal(10'000.0, 1);
    auto aligned = signal.audio;
    aligned.samples[500] = 0.1F;

    const auto result = evaluate_verification(
        aligned, signal, empty_alignment_info(), -12.0);

    CP_REQUIRE(result.failures.empty());
    CP_REQUIRE(contains_warning(
        result, CaptureWarning::equipment_decay_may_affect_capture));
}

CP_TEST_CASE("verification accepts a small early sweep without decay warning") {
    const auto signal = make_verification_signal(10'000.0, 1);
    constexpr std::size_t shift_frames = 10;
    auto aligned = signal.audio;
    aligned.samples.erase(
        aligned.samples.begin(), aligned.samples.begin() + shift_frames);
    aligned.samples.insert(aligned.samples.end(), shift_frames, 0.0F);

    const auto result = evaluate_verification(
        aligned, signal, empty_alignment_info(), -12.0);

    CP_REQUIRE(result.failures.empty());
    CP_REQUIRE(result.start_offset_frames == -static_cast<std::int64_t>(shift_frames));
    CP_REQUIRE(!contains_warning(
        result, CaptureWarning::equipment_decay_may_affect_capture));
}

CP_TEST_CASE("verification correlation tolerates polarity gain and saturation changes") {
    const auto signal = make_verification_signal(10'000.0, 1);
    auto aligned = signal.audio;
    for (auto frame = signal.sweep_frame_range.lower_bound;
         frame < signal.sweep_frame_range.upper_bound;
         ++frame) {
        auto& sample = aligned.samples[static_cast<std::size_t>(frame)];
        sample = std::max(-0.12F, std::min(0.12F, sample * -2.5F));
    }

    const auto result = evaluate_verification(
        aligned, signal, empty_alignment_info(), -12.0);

    CP_REQUIRE(result.failures.empty());
    CP_REQUIRE(result.start_offset_frames == 0);
    CP_REQUIRE(result.sweep->reliability == VerificationReliability::reliable);
    CP_REQUIRE(result.sweep->direct_score > 0.7);
}

CP_TEST_CASE("verification fails when sweep timing exceeds tolerance") {
    const auto signal = make_verification_signal(10'000.0, 1);
    constexpr std::size_t shift_frames = 180;
    auto aligned = signal.audio;
    aligned.samples.insert(aligned.samples.begin(), shift_frames, 0.0F);
    aligned.samples.resize(signal.audio.samples.size());

    const auto result = evaluate_verification(
        aligned, signal, empty_alignment_info(), -12.0);

    CP_REQUIRE(result.start_offset_frames == static_cast<std::int64_t>(shift_frames));
    CP_REQUIRE(result.max_timing_error_frames == static_cast<std::int64_t>(shift_frames));
    CP_REQUIRE(contains_failure(result, CaptureFailure::verification_timing_mismatch));
}

CP_TEST_CASE("verification keeps direct timing but reports a stronger delayed echo as ambiguous") {
    const auto signal = make_verification_signal(10'000.0, 1);
    auto aligned = signal.audio;
    const auto half_sweep = signal.sweep_frame_range.size() / 2;
    for (auto frame = signal.sweep_frame_range.lower_bound + half_sweep;
         frame < signal.sweep_frame_range.upper_bound;
         ++frame) {
        aligned.samples[static_cast<std::size_t>(frame)] = 0.0F;
    }
    const auto echo_start = signal.sweep_frame_range.upper_bound + 1'000;
    for (std::int64_t offset = 0; offset < signal.sweep_frame_range.size(); ++offset) {
        const auto source_frame = signal.sweep_frame_range.lower_bound + offset;
        const auto target_frame = echo_start + offset;
        if (target_frame >= aligned.frame_count()) continue;
        aligned.samples[static_cast<std::size_t>(target_frame)] =
            signal.audio.samples[static_cast<std::size_t>(source_frame)];
    }

    const auto result = evaluate_verification(
        aligned, signal, empty_alignment_info(), -12.0);

    CP_REQUIRE(result.failures.empty());
    CP_REQUIRE(result.start_offset_frames == 0);
    CP_REQUIRE(result.max_timing_error_frames == 0);
    CP_REQUIRE(result.ambiguous_match_count == 1);
    CP_REQUIRE(result.sweep->reliability == VerificationReliability::ambiguous);
    CP_REQUIRE(result.sweep->ambiguity_ratio.has_value());
    CP_REQUIRE(*result.sweep->ambiguity_ratio > 1.0);
    CP_REQUIRE(contains_warning(result, CaptureWarning::verification_ambiguous));
}

CP_TEST_CASE("verification ignores a weaker delayed sweep echo") {
    const auto signal = make_verification_signal(10'000.0, 1);
    auto aligned = signal.audio;
    const auto echo_start = signal.sweep_frame_range.upper_bound + 1'000;
    for (std::int64_t offset = 0; offset < signal.sweep_frame_range.size(); ++offset) {
        const auto source_frame = signal.sweep_frame_range.lower_bound + offset;
        const auto target_frame = echo_start + offset;
        if (target_frame >= aligned.frame_count()) continue;
        aligned.samples[static_cast<std::size_t>(target_frame)] +=
            signal.audio.samples[static_cast<std::size_t>(source_frame)] * 0.65F;
    }

    const auto result = evaluate_verification(
        aligned, signal, empty_alignment_info(), -12.0);

    CP_REQUIRE(result.failures.empty());
    CP_REQUIRE(result.start_offset_frames == 0);
    CP_REQUIRE(result.ambiguous_match_count == 0);
    CP_REQUIRE(result.sweep->reliability == VerificationReliability::reliable);
    CP_REQUIRE(!result.sweep->ambiguity_ratio.has_value());
    CP_REQUIRE(!contains_warning(result, CaptureWarning::verification_ambiguous));
}

CP_TEST_CASE("verification records digital clipping independently of sweep timing") {
    const auto signal = make_verification_signal(10'000.0, 1);
    const auto result = evaluate_verification(
        signal.audio, signal, empty_alignment_info(), -0.05);

    CP_REQUIRE(contains_failure(result, CaptureFailure::digital_clipping));
    CP_REQUIRE(result.start_offset_frames == 0);
}

CP_TEST_CASE("verification fails when the sweep signal is missing") {
    const auto signal = make_verification_signal(10'000.0, 1);
    auto silence = signal.audio;
    std::fill(silence.samples.begin(), silence.samples.end(), 0.0F);

    const auto result = evaluate_verification(
        silence, signal, empty_alignment_info(), -90.0);

    CP_REQUIRE(contains_failure(result, CaptureFailure::verification_signal_missing));
    CP_REQUIRE(!result.start_offset_frames.has_value());
    CP_REQUIRE(result.sweep.has_value());
    CP_REQUIRE(result.sweep->reliability == VerificationReliability::unmeasurable);
}
