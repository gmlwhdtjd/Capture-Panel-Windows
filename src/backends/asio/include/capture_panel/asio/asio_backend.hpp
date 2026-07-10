#pragma once

#include "capture_panel/core/backend.hpp"

namespace capture_panel::asio {

// Native Windows ASIO implementation. Driver IDs are stable registry-backed
// values in the form asio:{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}.
class AsioAudioBackend final : public IAudioCaptureBackend, public IAudioDeviceProvider {
public:
    [[nodiscard]] std::vector<AudioDevice> devices() const override;
    [[nodiscard]] AudioDevice device(const std::string& id) const override;
    [[nodiscard]] std::vector<AudioChannel> channels(
        const std::string& id,
        ChannelDirection direction) const override;
    void set_sample_rate(const std::string& id, double sample_rate) override;

    RawAudioCaptureResult capture(const RawAudioCaptureRequest& request) override;
};

} // namespace capture_panel::asio
