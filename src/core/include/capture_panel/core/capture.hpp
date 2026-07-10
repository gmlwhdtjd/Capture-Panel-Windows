#pragma once

#include "capture_panel/core/backend.hpp"
#include "capture_panel/core/types.hpp"

#include <memory>
#include <optional>

namespace capture_panel {

class CaptureService {
public:
    CaptureService(
        std::shared_ptr<IAudioDeviceProvider> device_provider,
        std::shared_ptr<IAudioCaptureBackend> capture_backend,
        CaptureEventHandler event_handler = {});

    [[nodiscard]] CapturePassResult capture(
        const CaptureConfiguration& configuration,
        const CapturePassOptions& options = {},
        std::shared_ptr<CancellationToken> cancellation = {});

    [[nodiscard]] CaptureVerificationResult verify_setup(
        const CaptureRoute& route,
        std::optional<double> sample_rate = std::nullopt,
        double output_trim_db = 0.0,
        double input_trim_db = 0.0,
        const CapturePassOptions& options = {},
        std::shared_ptr<CancellationToken> cancellation = {});

private:
    std::shared_ptr<IAudioDeviceProvider> device_provider_;
    std::shared_ptr<IAudioCaptureBackend> capture_backend_;
    CaptureEventHandler event_handler_;
};

} // namespace capture_panel
