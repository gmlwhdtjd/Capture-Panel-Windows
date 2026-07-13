#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <thread>
#include <utility>

namespace capture_panel::asio {

// ASIO callbacks do not carry a user-data pointer. This gate publishes the one
// active callback target while tying every reader to a count in the same atomic
// state word as the closed bit. A reader therefore either enters before close
// (and is waited for) or observes close and never dereferences the target.
template <typename Target>
class CallbackLifetimeGate final {
public:
    class Reader final {
    public:
        Reader(const Reader&) = delete;
        Reader& operator=(const Reader&) = delete;

        Reader(Reader&& other) noexcept
            : gate_(std::exchange(other.gate_, nullptr)),
              target_(std::exchange(other.target_, nullptr)) {}

        Reader& operator=(Reader&&) = delete;

        ~Reader() {
            if (gate_ != nullptr) {
                gate_->state_.fetch_sub(1U, std::memory_order_release);
            }
        }

        [[nodiscard]] Target* operator->() const noexcept { return target_; }
        [[nodiscard]] Target& get() const noexcept { return *target_; }

    private:
        friend class CallbackLifetimeGate;

        Reader(CallbackLifetimeGate& gate, Target& target) noexcept
            : gate_(&gate), target_(&target) {}

        CallbackLifetimeGate* gate_ = nullptr;
        Target* target_ = nullptr;
    };

    CallbackLifetimeGate() = default;
    CallbackLifetimeGate(const CallbackLifetimeGate&) = delete;
    CallbackLifetimeGate& operator=(const CallbackLifetimeGate&) = delete;

    [[nodiscard]] bool try_activate(Target& target) noexcept {
        Target* expected = nullptr;
        if (!owner_.compare_exchange_strong(
                expected,
                &target,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return false;
        }

        target_ = &target;
        state_.store(0U, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::optional<Reader> try_acquire() noexcept {
        auto state = state_.load(std::memory_order_relaxed);
        while ((state & closed_bit) == 0U) {
            if ((state & reader_mask) == reader_mask) return std::nullopt;
            if (state_.compare_exchange_weak(
                    state,
                    state + 1U,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                return Reader(*this, *target_);
            }
        }
        return std::nullopt;
    }

    void deactivate_and_wait() noexcept {
        if (owner_.load(std::memory_order_acquire) == nullptr) return;

        state_.fetch_or(closed_bit, std::memory_order_acq_rel);
        while ((state_.load(std::memory_order_acquire) & reader_mask) != 0U) {
            std::this_thread::yield();
        }
        target_ = nullptr;
        owner_.store(nullptr, std::memory_order_release);
    }

private:
    static constexpr std::uint32_t closed_bit = std::uint32_t{1} << 31U;
    static constexpr std::uint32_t reader_mask = closed_bit - 1U;

    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic<Target*>::is_always_lock_free);

    std::atomic<std::uint32_t> state_{closed_bit};
    std::atomic<Target*> owner_{nullptr};
    Target* target_ = nullptr;
};

} // namespace capture_panel::asio
