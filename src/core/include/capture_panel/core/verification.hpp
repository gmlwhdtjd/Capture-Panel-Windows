#pragma once

#include "capture_panel/core/constants.hpp"
#include "capture_panel/core/types.hpp"

#include <cstdint>

namespace capture_panel {

struct AudioFrameRange {
    std::int64_t lower_bound = 0;
    std::int64_t upper_bound = 0;

    [[nodiscard]] bool empty() const noexcept { return lower_bound >= upper_bound; }
    [[nodiscard]] std::int64_t size() const noexcept {
        return empty() ? 0 : upper_bound - lower_bound;
    }
};

struct VerificationSignal {
    AudioBuffer audio;
    WavFormat format;
    std::uint32_t channel_count = 0;
    AudioFrameRange leading_silence_frame_range;
    AudioFrameRange sweep_frame_range;
};

[[nodiscard]] VerificationSignal make_verification_signal(
    double sample_rate,
    std::uint32_t channel_count,
    double level_dbfs = constants::verification_signal::default_level_dbfs);

[[nodiscard]] AlignmentVerificationResult evaluate_verification(
    const AudioBuffer& aligned,
    const VerificationSignal& signal,
    const PayloadAlignmentInfo& alignment_info,
    double input_peak_dbfs);

} // namespace capture_panel
