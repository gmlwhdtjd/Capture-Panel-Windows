#include "test_framework.hpp"

#include "capture_panel/core/verification.hpp"
#include "capture_panel/core/errors.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
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

CP_TEST_CASE("verification signal rejects unsafe rates and channel dimensions") {
    for (const auto sample_rate : std::vector<double>{
             0.0,
             999.0,
             768'001.0,
             std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::max()}) {
        bool failed = false;
        try {
            static_cast<void>(make_verification_signal(sample_rate, 1));
        } catch (const CaptureError& error) {
            failed = error.code() == ErrorCode::unsupported_sample_rate;
        }
        CP_REQUIRE(failed);
    }

    bool bad_channels = false;
    try {
        static_cast<void>(make_verification_signal(
            48'000.0, std::numeric_limits<std::uint32_t>::max()));
    } catch (const CaptureError& error) {
        bad_channels = error.code() == ErrorCode::validation_failed;
    }
    CP_REQUIRE(bad_channels);
}

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

CP_TEST_CASE("verification uses recorded channel count when playback and recording differ") {
    const auto signal = make_verification_signal(10'000.0, 2);
    AudioBuffer mono_recording{
        .sample_rate = signal.audio.sample_rate,
        .channel_count = 1,
        .samples = std::vector<float>(
            static_cast<std::size_t>(signal.audio.frame_count()),
            0.0F),
    };
    for (std::int64_t frame = 0; frame < signal.audio.frame_count(); ++frame) {
        mono_recording.samples[static_cast<std::size_t>(frame)] =
            signal.audio.samples[static_cast<std::size_t>(frame) * signal.audio.channel_count];
    }

    const auto result = evaluate_verification(
        mono_recording, signal, empty_alignment_info(), -12.0);

    CP_REQUIRE(result.failures.empty());
    CP_REQUIRE(result.start_offset_frames == 0);
    CP_REQUIRE(result.sweep.has_value());
    CP_REQUIRE(result.sweep->direct_score > 0.99);
}

CP_TEST_CASE("verification correlation keeps identical and opposite-polarity record channels") {
    const auto signal = make_verification_signal(10'000.0, 2);
    for (const auto second_channel_polarity : {1.0F, -1.0F}) {
        auto aligned = signal.audio;
        for (std::int64_t frame = 0; frame < aligned.frame_count(); ++frame) {
            const auto first_index = static_cast<std::size_t>(frame) * 2U;
            aligned.samples[first_index + 1U] =
                aligned.samples[first_index] * second_channel_polarity;
        }

        const auto result = evaluate_verification(
            aligned, signal, empty_alignment_info(), -12.0);

        CP_REQUIRE(result.failures.empty());
        CP_REQUIRE(result.start_offset_frames == 0);
        CP_REQUIRE(result.sweep.has_value());
        CP_REQUIRE(result.sweep->reliability == VerificationReliability::reliable);
        CP_REQUIRE(result.sweep->direct_score > 0.99);
    }
}

CP_TEST_CASE("verification correlation ignores a louder uncorrelated record channel") {
    const auto signal = make_verification_signal(10'000.0, 2);
    auto aligned = signal.audio;
    std::uint32_t noise_state = 0xC0FFEEU;
    for (std::int64_t frame = 0; frame < aligned.frame_count(); ++frame) {
        noise_state = noise_state * 1'664'525U + 1'013'904'223U;
        const auto normalized = static_cast<float>((noise_state >> 8U) & 0x00FF'FFFFU)
                / static_cast<float>(0x00FF'FFFFU)
            * 2.0F - 1.0F;
        aligned.samples[static_cast<std::size_t>(frame) * 2U + 1U] = normalized * 0.7F;
    }

    const auto result = evaluate_verification(
        aligned, signal, empty_alignment_info(), -3.0);

    CP_REQUIRE(result.failures.empty());
    CP_REQUIRE(result.start_offset_frames == 0);
    CP_REQUIRE(result.sweep.has_value());
    CP_REQUIRE(result.sweep->reliability == VerificationReliability::reliable);
    CP_REQUIRE(result.sweep->direct_score > 0.99);
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

CP_TEST_CASE("verification keeps strong ambiguous evidence over a weak reliable coincidence") {
    const auto signal = make_verification_signal(10'000.0, 1);
    auto strong_ambiguous = signal.audio.samples;
    const auto half_sweep = signal.sweep_frame_range.size() / 2;
    for (auto frame = signal.sweep_frame_range.lower_bound + half_sweep;
         frame < signal.sweep_frame_range.upper_bound;
         ++frame) {
        strong_ambiguous[static_cast<std::size_t>(frame)] = 0.0F;
    }
    const auto echo_start = signal.sweep_frame_range.upper_bound + 1'000;
    for (std::int64_t offset = 0; offset < signal.sweep_frame_range.size(); ++offset) {
        const auto source_frame = signal.sweep_frame_range.lower_bound + offset;
        const auto target_frame = echo_start + offset;
        if (target_frame >= signal.audio.frame_count()) continue;
        strong_ambiguous[static_cast<std::size_t>(target_frame)] =
            signal.audio.samples[static_cast<std::size_t>(source_frame)];
    }

    std::vector<float> weak_coincidence(signal.audio.samples.size(), 0.0F);
    std::uint32_t noise_state = 0x51A7E123U;
    for (std::size_t index = 0; index < weak_coincidence.size(); ++index) {
        noise_state = noise_state * 1'664'525U + 1'013'904'223U;
        const auto normalized = static_cast<float>((noise_state >> 8U) & 0x00FF'FFFFU)
                / static_cast<float>(0x00FF'FFFFU)
            * 2.0F - 1.0F;
        weak_coincidence[index] = normalized * 0.5F
            + signal.audio.samples[index] * 0.2F;
    }

    AudioBuffer aligned{
        .sample_rate = signal.audio.sample_rate,
        .channel_count = 2,
        .samples = std::vector<float>(signal.audio.samples.size() * 2U, 0.0F),
    };
    for (std::size_t frame = 0; frame < strong_ambiguous.size(); ++frame) {
        aligned.samples[frame * 2U] = strong_ambiguous[frame];
        aligned.samples[frame * 2U + 1U] = weak_coincidence[frame];
    }

    const auto result = evaluate_verification(
        aligned, signal, empty_alignment_info(), -3.0);

    CP_REQUIRE(result.failures.empty());
    CP_REQUIRE(result.start_offset_frames == 0);
    CP_REQUIRE(result.sweep.has_value());
    CP_REQUIRE(result.sweep->direct_score > 0.5);
    CP_REQUIRE(result.sweep->reliability == VerificationReliability::ambiguous);
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
