#include "test_framework.hpp"

#include "capture_panel/backends/backend_router.hpp"
#include "capture_panel/core/errors.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace capture_panel;

class StubAudioBackend final : public IAudioDeviceProvider, public IAudioCaptureBackend {
public:
    explicit StubAudioBackend(std::string id, std::string name)
        : device_{
              .id = std::move(id),
              .name = std::move(name),
              .input_channels = 2,
              .output_channels = 3,
              .sample_rate = 48'000.0,
              .available = true,
          } {}

    [[nodiscard]] std::vector<AudioDevice> devices() const override {
        return {device_};
    }

    [[nodiscard]] AudioDevice device(const std::string& id) const override {
        validate_id(id);
        ++device_queries;
        return device_;
    }

    [[nodiscard]] std::vector<AudioChannel> channels(
        const std::string& id,
        ChannelDirection direction) const override {
        validate_id(id);
        ++channel_queries;
        const auto count = direction == ChannelDirection::input
            ? device_.input_channels
            : device_.output_channels;
        std::vector<AudioChannel> result;
        for (std::uint32_t index = 1; index <= count; ++index) {
            result.push_back({.index = index, .name = "Channel " + std::to_string(index)});
        }
        return result;
    }

    void set_sample_rate(const std::string& id, double sample_rate) override {
        validate_id(id);
        ++sample_rate_changes;
        device_.sample_rate = sample_rate;
    }

    RawAudioCaptureResult capture(const RawAudioCaptureRequest& request) override {
        validate_id(request.route.driver_id);
        ++captures;
        return {
            .recorded = AudioBuffer{
                .sample_rate = device_.sample_rate,
                .channel_count = 1,
                .samples = {capture_sample},
            },
            .pre_pad_frames = 0,
        };
    }

    mutable int channel_queries = 0;
    mutable int device_queries = 0;
    int sample_rate_changes = 0;
    int captures = 0;
    float capture_sample = 0.0F;

private:
    void validate_id(const std::string& id) const {
        if (id != device_.id) {
            throw CaptureError(ErrorCode::device_not_found, "stub driver not found");
        }
    }

    AudioDevice device_;
};

[[nodiscard]] bool is_device_not_found(const auto& operation) {
    try {
        operation();
    } catch (const CaptureError& error) {
        return error.code() == ErrorCode::device_not_found;
    }
    return false;
}

} // namespace

CP_TEST_CASE("BackendRouter combines fake and ASIO device providers") {
    auto fake = std::make_shared<StubAudioBackend>("fake:loopback", "Fake Loopback");
    auto asio = std::make_shared<StubAudioBackend>(
        "asio:{01234567-89AB-CDEF-0123-456789ABCDEF}",
        "ASIO Device");
    capture_panel::backends::BackendRouter router;
    router.add_backend(fake, fake);
    router.add_backend(asio, asio);

    const auto devices = router.devices();
    CP_REQUIRE(devices.size() == 2);
    CP_REQUIRE(devices[0].id == "fake:loopback");
    CP_REQUIRE(devices[1].id == "asio:{01234567-89AB-CDEF-0123-456789ABCDEF}");
    CP_REQUIRE(router.device(devices[0].id).name == "Fake Loopback");
    CP_REQUIRE(router.device(devices[1].id).name == "ASIO Device");
}

CP_TEST_CASE("BackendRouter delegates channel rate and capture operations by driver id") {
    auto fake = std::make_shared<StubAudioBackend>("fake:loopback", "Fake Loopback");
    auto asio = std::make_shared<StubAudioBackend>(
        "asio:{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}",
        "ASIO Device");
    asio->capture_sample = 0.25F;
    capture_panel::backends::BackendRouter router({
        {.device_provider = fake, .capture_backend = fake, .id_prefix = "fake:"},
        {.device_provider = asio, .capture_backend = asio, .id_prefix = "asio:"},
    });

    const auto asio_id = std::string("asio:{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}");
    const auto channels = router.channels(asio_id, ChannelDirection::output);
    CP_REQUIRE(channels.size() == 3);
    CP_REQUIRE(fake->channel_queries == 0);
    CP_REQUIRE(asio->channel_queries == 1);

    router.set_sample_rate(asio_id, 44'100.0);
    CP_REQUIRE(fake->sample_rate_changes == 0);
    CP_REQUIRE(asio->sample_rate_changes == 1);
    CP_REQUIRE_NEAR(router.device(asio_id).sample_rate, 44'100.0, 0.01);
    const auto device_queries_before_capture = asio->device_queries;

    const auto captured = router.capture({
        .route = {
            .driver_id = asio_id,
            .playback_channels = {9},
            .record_channels = {1},
        },
        .playback = AudioBuffer{
            .sample_rate = 44'100.0,
            .channel_count = 1,
            .samples = {0.5F},
        },
    });
    CP_REQUIRE(fake->captures == 0);
    CP_REQUIRE(asio->captures == 1);
    CP_REQUIRE(asio->device_queries == device_queries_before_capture);
    CP_REQUIRE(captured.recorded.samples.size() == 1);
    CP_REQUIRE_NEAR(captured.recorded.samples.front(), 0.25, 0.000001);
}

CP_TEST_CASE("BackendRouter reports unknown ids as device_not_found") {
    auto fake = std::make_shared<StubAudioBackend>("fake:loopback", "Fake Loopback");
    capture_panel::backends::BackendRouter router;
    router.add_backend(fake, fake);

    CP_REQUIRE(is_device_not_found([&] {
        static_cast<void>(router.device("unknown:driver"));
    }));
    CP_REQUIRE(is_device_not_found([&] {
        static_cast<void>(router.channels("unknown:driver", ChannelDirection::input));
    }));
    CP_REQUIRE(is_device_not_found([&] {
        router.set_sample_rate("unknown:driver", 48'000.0);
    }));
    CP_REQUIRE(is_device_not_found([&] {
        static_cast<void>(router.capture({
            .route = {.driver_id = "unknown:driver"},
        }));
    }));
}
