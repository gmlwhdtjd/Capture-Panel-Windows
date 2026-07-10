#pragma once

#include "capture_panel/core/types.hpp"

#include <functional>
#include <memory>
#include <vector>

namespace capture_panel {

struct RawAudioCaptureRequest {
    CaptureRoute route;
    AudioBuffer playback;
    double padding_seconds = 0.0;
    std::shared_ptr<CancellationToken> cancellation;
    std::function<void(std::int64_t, std::int64_t)> progress;
};

struct RawAudioCaptureResult {
    AudioBuffer recorded;
    std::int64_t pre_pad_frames = 0;
};

class IAudioCaptureBackend {
public:
    virtual ~IAudioCaptureBackend() = default;
    virtual RawAudioCaptureResult capture(const RawAudioCaptureRequest& request) = 0;
};

class IAudioDeviceProvider {
public:
    virtual ~IAudioDeviceProvider() = default;
    [[nodiscard]] virtual std::vector<AudioDevice> devices() const = 0;
    [[nodiscard]] virtual AudioDevice device(const std::string& id) const = 0;
    [[nodiscard]] virtual std::vector<AudioChannel> channels(
        const std::string& id,
        ChannelDirection direction) const = 0;
    virtual void set_sample_rate(const std::string& id, double sample_rate) = 0;
};

} // namespace capture_panel
