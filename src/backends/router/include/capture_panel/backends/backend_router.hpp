#pragma once

#include "capture_panel/core/backend.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace capture_panel::backends {

struct BackendRegistration {
    std::shared_ptr<IAudioDeviceProvider> device_provider;
    std::shared_ptr<IAudioCaptureBackend> capture_backend;
    // Optional side-effect-free ownership hint used by capture dispatch. For
    // example, "asio:" avoids opening a driver merely to identify its backend.
    std::string id_prefix;
};

// Combines independently implemented audio backends behind the core interfaces.
// A driver id is resolved by asking each provider in registration order; only a
// device_not_found error allows resolution to continue to the next provider.
class BackendRouter final : public IAudioDeviceProvider, public IAudioCaptureBackend {
public:
    BackendRouter() = default;
    explicit BackendRouter(std::vector<BackendRegistration> registrations);

    void add_backend(
        std::shared_ptr<IAudioDeviceProvider> device_provider,
        std::shared_ptr<IAudioCaptureBackend> capture_backend,
        std::string id_prefix = {});

    [[nodiscard]] std::vector<AudioDevice> devices() const override;
    [[nodiscard]] AudioDevice device(const std::string& id) const override;
    [[nodiscard]] std::vector<AudioChannel> channels(
        const std::string& id,
        ChannelDirection direction) const override;
    void set_sample_rate(const std::string& id, double sample_rate) override;

    RawAudioCaptureResult capture(const RawAudioCaptureRequest& request) override;

private:
    [[nodiscard]] const BackendRegistration& registration_for(
        const std::string& id) const;

    std::vector<BackendRegistration> registrations_;
};

} // namespace capture_panel::backends
