#pragma once

#include "capture_panel/core/types.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace capture_panel {

/// Parses one-based channel specifications such as "1,2" and "1-4".
/// Ordering and duplicates are preserved.
[[nodiscard]] std::vector<std::uint32_t> parse_channel_spec(std::string_view specification);

/// Validates one-based channel numbers against the number exposed by a device.
void validate_channels(
    std::span<const std::uint32_t> channels,
    std::uint32_t available_channel_count,
    ChannelDirection direction);

void validate_playback_channels(
    std::span<const std::uint32_t> channels,
    std::uint32_t available_channel_count);

void validate_record_channels(
    std::span<const std::uint32_t> channels,
    std::uint32_t available_channel_count);

} // namespace capture_panel
