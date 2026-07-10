#pragma once

#include "capture_panel/core/backend.hpp"

#include <cstdint>
#include <string_view>

namespace capture_panel::fake {

inline constexpr std::string_view loopback_device_id = "fake:loopback";

struct FakeBackendOptions {
    double sample_rate = 48'000.0;
    std::uint32_t input_channels = 8;
    std::uint32_t output_channels = 8;
    std::int64_t latency_frames = 0;
    double loopback_gain_db = 0.0;
    std::int64_t progress_block_frames = 256;
};

// Deterministic, in-memory full-duplex device used by tests and development
// machines without an ASIO driver. Playback channel N is looped back to
// recording channel N by route order; unmatched recording channels are silent.
class FakeAudioBackend final : public IAudioCaptureBackend, public IAudioDeviceProvider {
public:
    explicit FakeAudioBackend(FakeBackendOptions options = {});

    [[nodiscard]] const FakeBackendOptions& options() const noexcept { return options_; }
    void set_latency_frames(std::int64_t frames);
    void set_loopback_gain_db(double gain_db);

    [[nodiscard]] std::vector<AudioDevice> devices() const override;
    [[nodiscard]] AudioDevice device(const std::string& id) const override;
    [[nodiscard]] std::vector<AudioChannel> channels(
        const std::string& id,
        ChannelDirection direction) const override;
    void set_sample_rate(const std::string& id, double sample_rate) override;

    RawAudioCaptureResult capture(const RawAudioCaptureRequest& request) override;

private:
    void validate_device_id(const std::string& id) const;
    void validate_channels(
        const std::vector<std::uint32_t>& channels,
        std::uint32_t available,
        std::string_view direction) const;

    FakeBackendOptions options_;
};

} // namespace capture_panel::fake
