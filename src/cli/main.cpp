#include "cli.hpp"

#include "capture_panel/asio/asio_backend.hpp"
#include "capture_panel/backends/backend_router.hpp"
#include "capture_panel/fake/fake_backend.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] capture_panel::CancellationToken& console_cancellation() {
    // Console control handlers run on an operating-system thread and may still
    // be returning while wmain unwinds. Deliberately retain this token until
    // process teardown so a handler that passed the active check can never
    // dereference a destroyed object. wmain initializes it before registering
    // the handler.
    static auto* const token = new capture_panel::CancellationToken();
    return *token;
}

std::atomic_bool console_cancellation_active{false};
static_assert(std::atomic_bool::is_always_lock_free);

BOOL WINAPI console_control_handler(const DWORD event) noexcept {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT
        && event != CTRL_CLOSE_EVENT) {
        return FALSE;
    }
    if (console_cancellation_active.load(std::memory_order_acquire)) {
        console_cancellation().cancel();
        return TRUE;
    }
    return FALSE;
}

class ConsoleCancellationRegistration final {
public:
    ConsoleCancellationRegistration() {
        console_cancellation_active.store(true, std::memory_order_release);
        if (SetConsoleCtrlHandler(console_control_handler, TRUE) == FALSE) {
            console_cancellation_active.store(false, std::memory_order_release);
            throw std::runtime_error("Could not install the console cancellation handler.");
        }
    }

    ~ConsoleCancellationRegistration() {
        console_cancellation_active.store(false, std::memory_order_release);
        static_cast<void>(SetConsoleCtrlHandler(console_control_handler, FALSE));
    }

    ConsoleCancellationRegistration(const ConsoleCancellationRegistration&) = delete;
    ConsoleCancellationRegistration& operator=(const ConsoleCancellationRegistration&) = delete;
};

[[nodiscard]] std::string utf8_from_wide(std::wstring_view value) {
    if (value.empty()) return {};
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Command-line argument is too long.");
    }
    const auto input_size = static_cast<int>(value.size());
    const auto required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), input_size,
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        throw std::runtime_error("Could not decode a command-line argument.");
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), input_size,
            result.data(), required, nullptr, nullptr) != required) {
        throw std::runtime_error("Could not decode a command-line argument.");
    }
    return result;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    auto json_requested = false;
    for (int index = 1; index < argc; ++index) {
        if (std::wstring_view(argv[index]) == L"--json") {
            json_requested = true;
            break;
        }
    }
    try {
        SetConsoleOutputCP(CP_UTF8);
        auto cancellation = std::shared_ptr<capture_panel::CancellationToken>(
            &console_cancellation(),
            [](capture_panel::CancellationToken*) noexcept {});
        auto asio_backend = std::make_shared<capture_panel::asio::AsioAudioBackend>();
        auto fake_backend = std::make_shared<capture_panel::fake::FakeAudioBackend>();
        auto backends = std::make_shared<capture_panel::backends::BackendRouter>();
        backends->add_backend(asio_backend, asio_backend, "asio:");
        backends->add_backend(fake_backend, fake_backend, "fake:");
        std::vector<std::string> arguments;
        arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0U);
        for (int index = 1; index < argc; ++index) {
            arguments.push_back(utf8_from_wide(argv[index]));
        }
        std::optional<ConsoleCancellationRegistration> cancellation_registration;
        if (!arguments.empty()
            && (arguments.front() == "test" || arguments.front() == "run")) {
            cancellation_registration.emplace();
        }
        return capture_panel::cli::run_cli(
            arguments,
            {
                .device_provider = backends,
                .capture_backend = backends,
                .cancellation = cancellation,
            },
            std::cout,
            std::cerr);
    } catch (const std::exception& exception) {
        return capture_panel::cli::report_startup_error(
            exception.what(),
            json_requested,
            std::cout,
            std::cerr);
    }
}
