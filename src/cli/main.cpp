#include "cli.hpp"

#include "capture_panel/fake/fake_backend.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

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
    // Until the ASIO backend lands, the real CLI entry point uses the same
    // deterministic backend as CI. Replacing these two injected interfaces is
    // the only platform-specific change required by the command frontend.
    try {
        SetConsoleOutputCP(CP_UTF8);
        auto backend = std::make_shared<capture_panel::fake::FakeAudioBackend>();
        std::vector<std::string> arguments;
        arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0U);
        for (int index = 1; index < argc; ++index) {
            arguments.push_back(utf8_from_wide(argv[index]));
        }
        return capture_panel::cli::run_cli(
            arguments,
            {.device_provider = backend, .capture_backend = backend},
            std::cout,
            std::cerr);
    } catch (const std::exception& exception) {
        std::cerr << "Error: " << exception.what() << '\n';
        return 1;
    }
}
