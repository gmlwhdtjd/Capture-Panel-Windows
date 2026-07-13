#include "asio_callback_lifetime.hpp"
#include "asio_session_gate.hpp"
#include "test_framework.hpp"

#include <atomic>
#include <thread>

namespace capture_panel::test {

CP_TEST_CASE("ASIO callback lifetime gate drains entered readers before reuse") {
    struct Target {
        int value = 0;
    };

    asio::CallbackLifetimeGate<Target> gate;
    Target first{.value = 1};
    Target second{.value = 2};
    CP_REQUIRE(gate.try_activate(first));
    CP_REQUIRE(!gate.try_activate(second));

    auto held_reader = gate.try_acquire();
    CP_REQUIRE(held_reader.has_value());
    CP_REQUIRE(held_reader->get().value == 1);

    std::atomic_bool deactivation_started{false};
    std::atomic_bool deactivation_finished{false};
    std::thread deactivator([&] {
        deactivation_started.store(true, std::memory_order_release);
        gate.deactivate_and_wait();
        deactivation_finished.store(true, std::memory_order_release);
    });

    while (!deactivation_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    // Wait until close has won the atomic state transition. Temporary readers
    // acquired before that point release immediately at the end of the loop.
    while (gate.try_acquire().has_value()) {
        std::this_thread::yield();
    }
    const auto waited_for_reader = !deactivation_finished.load(std::memory_order_acquire);

    held_reader.reset();
    deactivator.join();
    CP_REQUIRE(waited_for_reader);
    CP_REQUIRE(deactivation_finished.load(std::memory_order_acquire));
    CP_REQUIRE(!gate.try_acquire().has_value());

    CP_REQUIRE(gate.try_activate(second));
    auto second_reader = gate.try_acquire();
    CP_REQUIRE(second_reader.has_value());
    CP_REQUIRE(second_reader->get().value == 2);
    second_reader.reset();
    gate.deactivate_and_wait();
}

CP_TEST_CASE("ASIO process session gate excludes concurrent driver operations") {
    auto first = asio::acquire_asio_session();
    std::atomic_bool second_try_succeeded{true};
    std::thread contender([&] {
        auto& mutex = asio::process_asio_session_mutex();
        const auto acquired = mutex.try_lock();
        second_try_succeeded.store(acquired, std::memory_order_release);
        if (acquired) mutex.unlock();
    });
    contender.join();
    CP_REQUIRE(!second_try_succeeded.load(std::memory_order_acquire));

    bool rejected_reentry = false;
    try {
        static_cast<void>(asio::acquire_asio_session());
    } catch (const CaptureError& error) {
        rejected_reentry = error.code() == ErrorCode::backend_failure;
    }
    CP_REQUIRE(rejected_reentry);

    first.unlock();
    auto& mutex = asio::process_asio_session_mutex();
    CP_REQUIRE(mutex.try_lock());
    mutex.unlock();
}

} // namespace capture_panel::test
