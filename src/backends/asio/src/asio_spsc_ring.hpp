#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace capture_panel::asio {

/// A bounded, allocation-free single-producer/single-consumer ring.
///
/// Exactly one thread may call the write functions and exactly one other
/// thread may call the read functions. Construction allocates all storage;
/// every operation afterwards is bounded to two memcpy calls and atomic cursor
/// publication, which makes the exact variants suitable for an ASIO callback.
template <typename T>
class SpscRing final {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

public:
    explicit SpscRing(const std::size_t capacity)
        : storage_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("An SPSC ring must have non-zero capacity.");
        }
    }

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    [[nodiscard]] std::size_t capacity() const noexcept { return storage_.size(); }

    [[nodiscard]] std::size_t available_to_read() const noexcept {
        const auto read = read_cursor_.value.load(std::memory_order_acquire);
        const auto write = write_cursor_.value.load(std::memory_order_acquire);
        return static_cast<std::size_t>(write - read);
    }

    [[nodiscard]] std::size_t available_to_write() const noexcept {
        return capacity() - available_to_read();
    }

    /// Writes as many values as currently fit and returns the count written.
    [[nodiscard]] std::size_t write(const std::span<const T> source) noexcept {
        const auto write = write_cursor_.value.load(std::memory_order_relaxed);
        const auto read = read_cursor_.value.load(std::memory_order_acquire);
        const auto used = static_cast<std::size_t>(write - read);
        const auto count = std::min(source.size(), capacity() - used);
        copy_into_storage(write, source.first(count));
        write_cursor_.value.store(write + count, std::memory_order_release);
        return count;
    }

    /// Writes the complete block, or leaves both storage visibility and the
    /// producer cursor unchanged when insufficient room is available.
    [[nodiscard]] bool write_exact(const std::span<const T> source) noexcept {
        if (source.empty()) return true;
        if (source.size() > capacity()) return false;

        const auto write = write_cursor_.value.load(std::memory_order_relaxed);
        const auto read = read_cursor_.value.load(std::memory_order_acquire);
        const auto used = static_cast<std::size_t>(write - read);
        if (capacity() - used < source.size()) return false;

        copy_into_storage(write, source);
        write_cursor_.value.store(write + source.size(), std::memory_order_release);
        return true;
    }

    /// Reads as many available values as fit in destination.
    [[nodiscard]] std::size_t read(const std::span<T> destination) noexcept {
        const auto read = read_cursor_.value.load(std::memory_order_relaxed);
        const auto write = write_cursor_.value.load(std::memory_order_acquire);
        const auto count = std::min(
            destination.size(),
            static_cast<std::size_t>(write - read));
        copy_from_storage(read, destination.first(count));
        read_cursor_.value.store(read + count, std::memory_order_release);
        return count;
    }

    /// Reads the complete block, or leaves the consumer cursor and destination
    /// unchanged when insufficient data is available.
    [[nodiscard]] bool read_exact(const std::span<T> destination) noexcept {
        if (destination.empty()) return true;
        if (destination.size() > capacity()) return false;

        const auto read = read_cursor_.value.load(std::memory_order_relaxed);
        const auto write = write_cursor_.value.load(std::memory_order_acquire);
        if (static_cast<std::size_t>(write - read) < destination.size()) return false;

        copy_from_storage(read, destination);
        read_cursor_.value.store(read + destination.size(), std::memory_order_release);
        return true;
    }

private:
    struct Cursor final {
        std::atomic<std::uint64_t> value{0};
        std::array<std::byte, 64 - sizeof(std::atomic<std::uint64_t>)> padding{};
    };
    static_assert(sizeof(Cursor) == 64);

    void copy_into_storage(
        const std::uint64_t cursor,
        const std::span<const T> source) noexcept {
        if (source.empty()) return;
        const auto start = static_cast<std::size_t>(cursor % capacity());
        const auto first = std::min(source.size(), capacity() - start);
        std::memcpy(storage_.data() + start, source.data(), first * sizeof(T));
        if (first < source.size()) {
            std::memcpy(
                storage_.data(),
                source.data() + first,
                (source.size() - first) * sizeof(T));
        }
    }

    void copy_from_storage(
        const std::uint64_t cursor,
        const std::span<T> destination) const noexcept {
        if (destination.empty()) return;
        const auto start = static_cast<std::size_t>(cursor % capacity());
        const auto first = std::min(destination.size(), capacity() - start);
        std::memcpy(destination.data(), storage_.data() + start, first * sizeof(T));
        if (first < destination.size()) {
            std::memcpy(
                destination.data() + first,
                storage_.data(),
                (destination.size() - first) * sizeof(T));
        }
    }

    std::vector<T> storage_;
    Cursor read_cursor_;
    // Keeping the two cursors at least two cache lines apart avoids false
    // sharing without requiring an over-aligned type (and its ABI padding).
    Cursor cursor_separation_;
    Cursor write_cursor_;
};

} // namespace capture_panel::asio
