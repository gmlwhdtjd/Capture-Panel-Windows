#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace capture_panel::asio {

struct AsioFrameBlock {
    std::int64_t start_frame = 0;
    std::size_t frame_count = 0;
};

// Pure state machine for the ASIO double-buffer timeline. Output B is taken
// once before start. The first input callback is invalid and consumes no
// logical recording frames.
class AsioBufferTimeline final {
public:
    AsioBufferTimeline(
        const std::int64_t total_frame_count,
        const std::size_t buffer_frame_count) noexcept
        : total_frame_count_(std::max<std::int64_t>(0, total_frame_count)),
          buffer_frame_count_(buffer_frame_count) {}

    [[nodiscard]] AsioFrameBlock next_output_block() noexcept {
        const auto block = remaining_block(output_frame_cursor_);
        output_frame_cursor_ += static_cast<std::int64_t>(block.frame_count);
        return block;
    }

    [[nodiscard]] std::optional<AsioFrameBlock> next_input_block() noexcept {
        if (first_input_callback_) {
            first_input_callback_ = false;
            return std::nullopt;
        }
        const auto block = remaining_block(recorded_frame_cursor_);
        recorded_frame_cursor_ += static_cast<std::int64_t>(block.frame_count);
        return block;
    }

    [[nodiscard]] std::int64_t recorded_frame_count() const noexcept {
        return recorded_frame_cursor_;
    }

    [[nodiscard]] bool recording_complete() const noexcept {
        return recorded_frame_cursor_ >= total_frame_count_;
    }

private:
    [[nodiscard]] AsioFrameBlock remaining_block(
        const std::int64_t cursor) const noexcept {
        const auto remaining = std::max<std::int64_t>(0, total_frame_count_ - cursor);
        const auto count = static_cast<std::size_t>(std::min<std::int64_t>(
            remaining,
            static_cast<std::int64_t>(buffer_frame_count_)));
        return {.start_frame = cursor, .frame_count = count};
    }

    std::int64_t total_frame_count_ = 0;
    std::size_t buffer_frame_count_ = 0;
    std::int64_t output_frame_cursor_ = 0;
    std::int64_t recorded_frame_cursor_ = 0;
    bool first_input_callback_ = true;
};

} // namespace capture_panel::asio
