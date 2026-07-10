#pragma once

#include "asio_driver_registry.hpp"
#include "capture_panel/core/errors.hpp"

#include <string>
#include <string_view>

struct IASIO;

namespace capture_panel::asio {

[[nodiscard]] AsioDriverRegistration find_asio_driver(std::string_view device_id);

void require_asio_result(
    IASIO& driver,
    long result,
    std::string_view operation,
    ErrorCode error_code = ErrorCode::backend_failure);

} // namespace capture_panel::asio
