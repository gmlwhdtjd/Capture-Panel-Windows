#include "capture_panel/core/channels.hpp"

#include "capture_panel/core/errors.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>

namespace capture_panel {
namespace {

constexpr std::uint64_t maximum_expanded_channels = 65'536;

[[nodiscard]] std::string_view trim(const std::string_view value) noexcept {
    auto first = value.begin();
    while (first != value.end() && std::isspace(static_cast<unsigned char>(*first)) != 0) {
        ++first;
    }
    auto last = value.end();
    while (last != first && std::isspace(static_cast<unsigned char>(*(last - 1))) != 0) {
        --last;
    }
    return {first, last};
}

[[noreturn]] void throw_invalid_spec(const std::string_view value) {
    throw CaptureError(
        ErrorCode::invalid_channel_specification,
        "Invalid channel specification: '" + std::string(value) + "'");
}

[[nodiscard]] std::uint32_t parse_positive_channel(const std::string_view value) {
    const auto token = trim(value);
    if (token.empty()) throw_invalid_spec(value);

    std::uint32_t channel = 0;
    const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), channel);
    if (error != std::errc{} || end != token.data() + token.size() || channel == 0) {
        throw_invalid_spec(token);
    }
    return channel;
}

} // namespace

std::vector<std::uint32_t> parse_channel_spec(const std::string_view specification) {
    if (trim(specification).empty()) throw_invalid_spec(specification);

    std::vector<std::uint32_t> channels;
    std::size_t part_begin = 0;
    while (part_begin <= specification.size()) {
        const auto comma = specification.find(',', part_begin);
        const auto part_end = comma == std::string_view::npos ? specification.size() : comma;
        const auto part = trim(specification.substr(part_begin, part_end - part_begin));
        if (part.empty()) throw_invalid_spec(specification);

        const auto hyphen = part.find('-');
        if (hyphen == std::string_view::npos) {
            if (channels.size() >= maximum_expanded_channels) {
                throw_invalid_spec(specification);
            }
            channels.push_back(parse_positive_channel(part));
        } else {
            if (part.find('-', hyphen + 1) != std::string_view::npos) {
                throw_invalid_spec(part);
            }
            const auto first = parse_positive_channel(part.substr(0, hyphen));
            const auto last = parse_positive_channel(part.substr(hyphen + 1));
            if (last < first) throw_invalid_spec(part);

            const auto count = static_cast<std::uint64_t>(last) - first + 1U;
            // This is well beyond any realistic ASIO channel count and bounds CLI input memory.
            if (count > maximum_expanded_channels
                || static_cast<std::uint64_t>(channels.size())
                        > maximum_expanded_channels - count
                || count > static_cast<std::uint64_t>(channels.max_size() - channels.size())) {
                throw_invalid_spec(part);
            }
            channels.reserve(channels.size() + static_cast<std::size_t>(count));
            for (auto channel = first;; ++channel) {
                channels.push_back(channel);
                if (channel == last) break;
            }
        }

        if (comma == std::string_view::npos) break;
        part_begin = comma + 1;
    }

    if (channels.empty()) throw_invalid_spec(specification);
    return channels;
}

void validate_channels(
    const std::span<const std::uint32_t> channels,
    const std::uint32_t available_channel_count,
    const ChannelDirection direction) {
    const auto label = direction == ChannelDirection::output ? "Playback" : "Record";
    const auto device_direction = direction == ChannelDirection::output ? "output" : "input";

    if (channels.empty()) {
        throw CaptureError(
            ErrorCode::validation_failed,
            std::string(label) + " channels cannot be empty");
    }

    const auto invalid_zero = std::find(channels.begin(), channels.end(), 0U);
    if (invalid_zero != channels.end()) {
        throw CaptureError(
            ErrorCode::validation_failed,
            std::string(label) + " channels are one-based and cannot contain zero");
    }

    const auto maximum = *std::max_element(channels.begin(), channels.end());
    if (maximum > available_channel_count) {
        std::ostringstream message;
        message << label << " channel " << maximum << " exceeds device "
                << device_direction << " channels (" << available_channel_count << ')';
        throw CaptureError(ErrorCode::validation_failed, message.str());
    }

    std::unordered_set<std::uint32_t> unique_channels;
    unique_channels.reserve(channels.size());
    for (const auto channel : channels) {
        if (!unique_channels.insert(channel).second) {
            std::ostringstream message;
            message << label << " channels cannot contain duplicate channel " << channel;
            throw CaptureError(ErrorCode::validation_failed, message.str());
        }
    }
}

void validate_playback_channels(
    const std::span<const std::uint32_t> channels,
    const std::uint32_t available_channel_count) {
    validate_channels(channels, available_channel_count, ChannelDirection::output);
}

void validate_record_channels(
    const std::span<const std::uint32_t> channels,
    const std::uint32_t available_channel_count) {
    validate_channels(channels, available_channel_count, ChannelDirection::input);
}

} // namespace capture_panel
