#include "asio_spsc_ring.hpp"
#include "test_framework.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

namespace capture_panel::test {

CP_TEST_CASE("ASIO SPSC ring preserves wrapped sample order") {
    asio::SpscRing<float> ring(5);
    const std::array first{1.0F, 2.0F, 3.0F, 4.0F};
    CP_REQUIRE(ring.write(first) == first.size());

    std::array<float, 3> consumed{};
    CP_REQUIRE(ring.read(consumed) == consumed.size());
    CP_REQUIRE(consumed[0] == 1.0F);
    CP_REQUIRE(consumed[2] == 3.0F);

    const std::array second{5.0F, 6.0F, 7.0F, 8.0F};
    CP_REQUIRE(ring.write(second) == second.size());
    CP_REQUIRE(ring.available_to_read() == 5);

    std::array<float, 5> result{};
    CP_REQUIRE(ring.read_exact(result));
    const std::array expected{4.0F, 5.0F, 6.0F, 7.0F, 8.0F};
    CP_REQUIRE(result == expected);
}

CP_TEST_CASE("ASIO SPSC exact operations never publish partial blocks") {
    asio::SpscRing<float> ring(4);
    const std::array first{1.0F, 2.0F, 3.0F};
    const std::array too_large{4.0F, 5.0F};
    CP_REQUIRE(ring.write_exact(first));
    CP_REQUIRE(!ring.write_exact(too_large));
    CP_REQUIRE(ring.available_to_read() == first.size());

    std::array<float, 4> unavailable{9.0F, 9.0F, 9.0F, 9.0F};
    const std::array<float, 4> expected_unavailable{9.0F, 9.0F, 9.0F, 9.0F};
    CP_REQUIRE(!ring.read_exact(unavailable));
    CP_REQUIRE(unavailable == expected_unavailable);

    std::array<float, 3> result{};
    CP_REQUIRE(ring.read_exact(result));
    CP_REQUIRE(result == first);
    CP_REQUIRE(ring.available_to_read() == 0);
}

CP_TEST_CASE("ASIO SPSC ring transfers a long concurrent sequence") {
    constexpr std::uint32_t value_count = 200'000;
    asio::SpscRing<std::uint32_t> ring(257);
    std::atomic_bool corrupt{false};

    std::thread producer([&] {
        std::uint32_t next = 0;
        std::array<std::uint32_t, 31> values{};
        while (next < value_count) {
            const auto count = std::min<std::size_t>(values.size(), value_count - next);
            for (std::size_t index = 0; index < count; ++index) {
                values[index] = next + static_cast<std::uint32_t>(index);
            }
            const auto written = ring.write(std::span(values).first(count));
            next += static_cast<std::uint32_t>(written);
            if (written == 0) std::this_thread::yield();
        }
    });

    std::uint32_t expected = 0;
    std::array<std::uint32_t, 37> values{};
    while (expected < value_count) {
        const auto read = ring.read(values);
        if (read == 0) {
            std::this_thread::yield();
            continue;
        }
        for (std::size_t index = 0; index < read; ++index) {
            if (values[index] != expected++) corrupt.store(true, std::memory_order_relaxed);
        }
    }
    producer.join();

    CP_REQUIRE(!corrupt.load(std::memory_order_relaxed));
    CP_REQUIRE(ring.available_to_read() == 0);
}

} // namespace capture_panel::test
