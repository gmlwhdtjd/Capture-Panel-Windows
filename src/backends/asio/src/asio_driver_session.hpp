#pragma once

#include "asio_driver_registry.hpp"

#include <memory>
#include <string>
#include <string_view>

struct IASIO;

namespace capture_panel::asio {

class AsioDriverSession final {
public:
    explicit AsioDriverSession(const AsioDriverRegistration& registration);
    ~AsioDriverSession();

    AsioDriverSession(const AsioDriverSession&) = delete;
    AsioDriverSession& operator=(const AsioDriverSession&) = delete;
    AsioDriverSession(AsioDriverSession&&) = delete;
    AsioDriverSession& operator=(AsioDriverSession&&) = delete;

    [[nodiscard]] IASIO& driver() const noexcept;
    [[nodiscard]] std::string driver_name() const;
    [[nodiscard]] std::string driver_error_message() const;

    // ASIO control panels and a few drivers depend on a live top-level HWND.
    // The synchronous backend calls this from its control-thread wait loop.
    void pump_messages() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] bool asio_result_succeeded(long result) noexcept;
[[nodiscard]] std::string asio_result_name(long result);
[[nodiscard]] std::string asio_text_to_utf8(std::string_view value);

} // namespace capture_panel::asio
