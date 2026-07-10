#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace capture_panel {

enum class ErrorCode {
    file_not_found,
    wav_read,
    wav_write,
    unsupported_format,
    unsupported_bit_depth,
    unsupported_sample_rate,
    device_not_found,
    invalid_channel_specification,
    validation_failed,
    alignment_failed,
    capture_cancelled,
    capture_timed_out,
    backend_failure,
};

class CaptureError final : public std::runtime_error {
public:
    CaptureError(ErrorCode code, std::string message)
        : std::runtime_error(std::move(message)), code_(code) {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }

private:
    ErrorCode code_;
};

} // namespace capture_panel
