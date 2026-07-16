#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace capture_panel::constants {

namespace impulse {
inline constexpr float adaptive_threshold_ratio = 0.5F;
inline constexpr float minimum_threshold = 0.005F;
inline constexpr double marker_latency_spread_threshold = 10.0;
inline constexpr double marker_interval_error_ppm_threshold = 1'000.0;
inline constexpr std::size_t minimum_impulses_for_analysis = 2;
} // namespace impulse

namespace alignment {
inline constexpr double padding_seconds = 0.5;
inline constexpr double marker_to_payload_silence_seconds = 2.0;
inline constexpr std::size_t impulse_count = 5;
inline constexpr double impulse_interval_seconds = 0.1;
inline constexpr double impulse_level_dbfs = -12.0;
} // namespace alignment

namespace marker {
inline constexpr double warning_detected_marker_ratio = 0.75;
inline constexpr double minimum_detected_marker_ratio = 0.45;
inline constexpr double search_radius_seconds = 0.60;
inline constexpr std::size_t minimum_search_radius_frames = 64;
inline constexpr double marker_tail_seconds = 0.08;
inline constexpr std::size_t minimum_marker_tail_frames = 8;
inline constexpr double negative_latency_penalty_multiplier = 10.0;
inline constexpr double late_echo_tie_breaker_weight = 0.001;
inline constexpr double fit_score_epsilon = 0.000001;
inline constexpr double sequence_match_tolerance_seconds = 0.035;
inline constexpr std::size_t minimum_sequence_match_tolerance_frames = 8;
inline constexpr double missing_marker_penalty_seconds = 0.02;
inline constexpr double minimum_missing_marker_penalty_frames = 16.0;
// A marker window containing more distinct transients than this is too noisy
// to align safely. Bounding it also keeps the gap-aware sequence matcher's
// work deterministic for malformed or adversarial input.
inline constexpr std::size_t maximum_sequence_candidates = 512;
} // namespace marker

namespace verification_signal {
inline constexpr double default_level_dbfs = -12.0;
inline constexpr std::uint32_t maximum_channel_count = 256;
inline constexpr double leading_silence_seconds = 0.1;
inline constexpr double sweep_seconds = 0.3;
inline constexpr double trailing_silence_seconds = 0.5;
inline constexpr double start_frequency = 80.0;
inline constexpr double maximum_end_frequency = 18'000.0;
inline constexpr double nyquist_end_frequency_ratio = 0.45;
inline constexpr double fade_seconds = 0.01;
} // namespace verification_signal

namespace verification_evaluation {
inline constexpr double clipping_threshold_dbfs = -0.1;
inline constexpr double missing_signal_peak_threshold = 0.001;
inline constexpr std::size_t minimum_timing_tolerance_frames = 8;
inline constexpr double timing_tolerance_seconds = 0.01;
inline constexpr double high_timing_error_warning_ratio = 0.5;
inline constexpr double leading_silence_peak_relative_dbfs = -18.0;
inline constexpr double leading_silence_rms_relative_dbfs = -30.0;
} // namespace verification_evaluation

namespace verification_sweep {
inline constexpr double search_radius_seconds = 0.05;
inline constexpr std::size_t minimum_search_radius_frames = 64;
inline constexpr double minimum_direct_score = 0.05;
inline constexpr std::size_t maximum_coarse_step_frames = 32;
inline constexpr std::size_t coarse_reference_frames_per_step = 600;
inline constexpr double separate_peak_seconds = 0.01;
inline constexpr std::size_t minimum_separate_peak_frames = 8;
inline constexpr double ambiguity_ratio_threshold = 1.0;
inline constexpr double recorded_energy_floor = 0.0000001;
} // namespace verification_sweep

namespace audio {
inline constexpr double fallback_sample_rate = 48'000.0;
inline constexpr double minimum_supported_sample_rate = 1'000.0;
inline constexpr double maximum_supported_sample_rate = 768'000.0;
inline constexpr double capture_timeout_margin_seconds = 5.0;
inline constexpr double capture_timeout_minimum_seconds = 10.0;
} // namespace audio

namespace gain {
inline constexpr double output_minimum_db = -24.0;
inline constexpr double output_maximum_db = 0.0;
inline constexpr double input_minimum_db = -18.0;
inline constexpr double input_maximum_db = 12.0;
} // namespace gain

} // namespace capture_panel::constants
