#include "capture_panel/backends/backend_router.hpp"

#include "capture_panel/core/errors.hpp"

#include <unordered_set>
#include <utility>

namespace capture_panel::backends {
namespace {

[[noreturn]] void throw_device_not_found(const std::string& id) {
    throw CaptureError(ErrorCode::device_not_found, "Audio driver not found: " + id);
}

void validate_registration(const BackendRegistration& registration) {
    if (!registration.device_provider || !registration.capture_backend) {
        throw CaptureError(
            ErrorCode::validation_failed,
            "A backend registration requires a device provider and capture backend.");
    }
}

[[nodiscard]] bool is_device_not_found(const CaptureError& error) noexcept {
    return error.code() == ErrorCode::device_not_found;
}

} // namespace

BackendRouter::BackendRouter(std::vector<BackendRegistration> registrations)
    : registrations_(std::move(registrations)) {
    for (const auto& registration : registrations_) {
        validate_registration(registration);
    }
}

void BackendRouter::add_backend(
    std::shared_ptr<IAudioDeviceProvider> device_provider,
    std::shared_ptr<IAudioCaptureBackend> capture_backend,
    std::string id_prefix) {
    BackendRegistration registration{
        .device_provider = std::move(device_provider),
        .capture_backend = std::move(capture_backend),
        .id_prefix = std::move(id_prefix),
    };
    validate_registration(registration);
    registrations_.push_back(std::move(registration));
}

std::vector<AudioDevice> BackendRouter::devices() const {
    std::vector<AudioDevice> result;
    std::unordered_set<std::string> known_ids;
    for (const auto& registration : registrations_) {
        for (auto device : registration.device_provider->devices()) {
            if (known_ids.insert(device.id).second) {
                result.push_back(std::move(device));
            }
        }
    }
    return result;
}

AudioDevice BackendRouter::device(const std::string& id) const {
    for (const auto& registration : registrations_) {
        try {
            return registration.device_provider->device(id);
        } catch (const CaptureError& error) {
            if (!is_device_not_found(error)) throw;
        }
    }
    throw_device_not_found(id);
}

std::vector<AudioChannel> BackendRouter::channels(
    const std::string& id,
    ChannelDirection direction) const {
    for (const auto& registration : registrations_) {
        try {
            return registration.device_provider->channels(id, direction);
        } catch (const CaptureError& error) {
            if (!is_device_not_found(error)) throw;
        }
    }
    throw_device_not_found(id);
}

void BackendRouter::set_sample_rate(const std::string& id, double sample_rate) {
    for (const auto& registration : registrations_) {
        try {
            registration.device_provider->set_sample_rate(id, sample_rate);
            return;
        } catch (const CaptureError& error) {
            if (!is_device_not_found(error)) throw;
        }
    }
    throw_device_not_found(id);
}

RawAudioCaptureResult BackendRouter::capture(const RawAudioCaptureRequest& request) {
    return registration_for(request.route.driver_id).capture_backend->capture(request);
}

const BackendRegistration& BackendRouter::registration_for(const std::string& id) const {
    for (const auto& registration : registrations_) {
        if (!registration.id_prefix.empty()
            && std::string_view(id).starts_with(registration.id_prefix)) {
            return registration;
        }
    }
    for (const auto& registration : registrations_) {
        if (!registration.id_prefix.empty()) continue;
        try {
            static_cast<void>(registration.device_provider->device(id));
            return registration;
        } catch (const CaptureError& error) {
            if (!is_device_not_found(error)) throw;
        }
    }
    throw_device_not_found(id);
}

} // namespace capture_panel::backends
