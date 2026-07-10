#include "capture_panel/asio/asio_backend.hpp"

#include "asio_backend_helpers.hpp"
#include "asio_driver_registry.hpp"
#include "asio_driver_session.hpp"
#include "capture_panel/core/errors.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>

#include "iasiodrv.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace capture_panel::asio {
namespace {

[[nodiscard]] std::string_view bounded_asio_text(
    const char* text,
    const std::size_t capacity) noexcept {
    std::size_t size = 0;
    while (size < capacity && text[size] != '\0') ++size;
    return {text, size};
}

[[nodiscard]] std::uint32_t checked_channel_count(
    const long value,
    const std::string_view direction) {
    if (value < 0) {
        throw CaptureError(
            ErrorCode::backend_failure,
            "ASIO returned an invalid " + std::string(direction) + " channel count.");
    }
    return static_cast<std::uint32_t>(value);
}

struct DriverCapabilities {
    std::uint32_t input_channels = 0;
    std::uint32_t output_channels = 0;
    double sample_rate = 0.0;
};

[[nodiscard]] DriverCapabilities query_capabilities(AsioDriverSession& session) {
    long inputs = 0;
    long outputs = 0;
    require_asio_result(
        session.driver(),
        session.driver().getChannels(&inputs, &outputs),
        "query channels");

    ASIOSampleRate sample_rate = 0.0;
    if (!asio_result_succeeded(session.driver().getSampleRate(&sample_rate))
        || !std::isfinite(sample_rate)
        || sample_rate <= 0.0) {
        sample_rate = 0.0;
    }
    return {
        .input_channels = checked_channel_count(inputs, "input"),
        .output_channels = checked_channel_count(outputs, "output"),
        .sample_rate = sample_rate,
    };
}

[[nodiscard]] std::string display_name(
    const AsioDriverRegistration& registration,
    AsioDriverSession& session) {
    auto name = session.driver_name();
    if (name.empty()) name = registration.description;
    if (name.empty()) name = registration.name;
    return name;
}

[[nodiscard]] AudioDevice probe_driver(const AsioDriverRegistration& registration) {
    try {
        AsioDriverSession session(registration);
        const auto capabilities = query_capabilities(session);
        return {
            .id = registration.id,
            .name = display_name(registration, session),
            .input_channels = capabilities.input_channels,
            .output_channels = capabilities.output_channels,
            .sample_rate = capabilities.sample_rate,
            .available = true,
            .status = "ASIO",
        };
    } catch (const std::exception& error) {
        return {
            .id = registration.id,
            .name = registration.description.empty()
                ? registration.name
                : registration.description,
            .input_channels = 0,
            .output_channels = 0,
            .sample_rate = 0.0,
            .available = false,
            .status = error.what(),
        };
    }
}

[[nodiscard]] std::string channel_fallback_name(
    const ChannelDirection direction,
    const std::uint32_t one_based_index) {
    return std::string(direction == ChannelDirection::input ? "Input " : "Output ")
        + std::to_string(one_based_index);
}

} // namespace

AsioDriverRegistration find_asio_driver(const std::string_view device_id) {
    const auto clsid = parse_asio_device_id(device_id);
    if (!clsid.has_value()) {
        throw CaptureError(
            ErrorCode::device_not_found,
            "Invalid ASIO driver ID: " + std::string(device_id));
    }
    auto registrations = enumerate_asio_drivers();
    const auto found = std::ranges::find(
        registrations, *clsid, &AsioDriverRegistration::clsid);
    if (found == registrations.end()) {
        throw CaptureError(
            ErrorCode::device_not_found,
            "ASIO driver not found: " + std::string(device_id));
    }
    return *found;
}

void require_asio_result(
    IASIO& driver,
    const long result,
    const std::string_view operation,
    const ErrorCode error_code) {
    if (asio_result_succeeded(result)) return;

    char driver_message[124]{};
    driver.getErrorMessage(driver_message);
    const auto bounded = bounded_asio_text(driver_message, sizeof(driver_message));
    const auto detail = asio_text_to_utf8(bounded);
    std::string message = "Could not " + std::string(operation) + " ("
        + asio_result_name(result) + ')';
    if (!detail.empty()) message += ": " + detail;
    throw CaptureError(error_code, std::move(message));
}

std::vector<AudioDevice> AsioAudioBackend::devices() const {
    const auto registrations = enumerate_asio_drivers();
    std::vector<AudioDevice> result;
    result.reserve(registrations.size());
    for (const auto& registration : registrations) {
        result.push_back(probe_driver(registration));
    }
    return result;
}

AudioDevice AsioAudioBackend::device(const std::string& id) const {
    return probe_driver(find_asio_driver(id));
}

std::vector<AudioChannel> AsioAudioBackend::channels(
    const std::string& id,
    const ChannelDirection direction) const {
    const auto registration = find_asio_driver(id);
    AsioDriverSession session(registration);
    const auto capabilities = query_capabilities(session);
    const auto channel_count = direction == ChannelDirection::input
        ? capabilities.input_channels
        : capabilities.output_channels;

    std::vector<AudioChannel> result;
    result.reserve(channel_count);
    for (std::uint32_t offset = 0; offset < channel_count; ++offset) {
        ASIOChannelInfo info{};
        info.channel = static_cast<long>(offset);
        info.isInput = direction == ChannelDirection::input ? ASIOTrue : ASIOFalse;
        require_asio_result(
            session.driver(),
            session.driver().getChannelInfo(&info),
            direction == ChannelDirection::input
                ? "query an ASIO input channel"
                : "query an ASIO output channel");
        const auto native_name = bounded_asio_text(info.name, sizeof(info.name));
        auto name = asio_text_to_utf8(native_name);
        const auto one_based_index = offset + 1U;
        if (name.empty()) name = channel_fallback_name(direction, one_based_index);
        result.push_back({.index = one_based_index, .name = std::move(name)});
    }
    return result;
}

void AsioAudioBackend::set_sample_rate(
    const std::string& id,
    const double sample_rate) {
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0) {
        throw CaptureError(
            ErrorCode::unsupported_sample_rate,
            "ASIO sample rate must be a positive finite value.");
    }
    const auto registration = find_asio_driver(id);
    AsioDriverSession session(registration);

    ASIOSampleRate current = 0.0;
    if (asio_result_succeeded(session.driver().getSampleRate(&current))
        && std::isfinite(current)
        && std::abs(current - sample_rate) <= 0.5) {
        return;
    }

    require_asio_result(
        session.driver(),
        session.driver().canSampleRate(sample_rate),
        "select the requested ASIO sample rate",
        ErrorCode::unsupported_sample_rate);
    require_asio_result(
        session.driver(),
        session.driver().setSampleRate(sample_rate),
        "set the ASIO sample rate",
        ErrorCode::unsupported_sample_rate);

    current = 0.0;
    require_asio_result(
        session.driver(),
        session.driver().getSampleRate(&current),
        "verify the ASIO sample rate",
        ErrorCode::unsupported_sample_rate);
    if (!std::isfinite(current) || std::abs(current - sample_rate) > 0.5) {
        throw CaptureError(
            ErrorCode::unsupported_sample_rate,
            "The ASIO driver did not apply the requested sample rate.");
    }
}

} // namespace capture_panel::asio
