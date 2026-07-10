#include "asio_driver_session.hpp"

#include "capture_panel/core/errors.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>

#include "iasiodrv.h"

#include <array>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace capture_panel::asio {
namespace {

constexpr wchar_t host_window_class_name[] = L"CapturePanelAsioHostWindow";

[[nodiscard]] std::string utf8_from_wide(const std::wstring_view value) {
    if (value.empty()) return {};
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return "Windows error text was too long";
    }
    const auto length = static_cast<int>(value.size());
    const auto required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length,
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return "Windows error text could not be encoded";
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length,
            result.data(), required, nullptr, nullptr) != required) {
        return "Windows error text could not be encoded";
    }
    return result;
}

[[nodiscard]] std::string windows_error_message(const HRESULT result) {
    wchar_t* allocated = nullptr;
    const auto length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(result),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&allocated),
        0,
        nullptr);
    std::wstring text;
    if (length > 0 && allocated != nullptr) {
        text.assign(allocated, allocated + length);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) {
            text.pop_back();
        }
    }
    if (allocated != nullptr) LocalFree(allocated);
    if (!text.empty()) return utf8_from_wide(text);

    std::ostringstream fallback;
    fallback << "HRESULT 0x" << std::hex << std::uppercase
             << static_cast<unsigned long>(result);
    return fallback.str();
}

[[nodiscard]] std::wstring wide_ascii(const std::string_view text) {
    return {text.begin(), text.end()};
}

[[nodiscard]] std::string_view bounded_asio_text(
    const char* text,
    const std::size_t capacity) noexcept {
    std::size_t size = 0;
    while (size < capacity && text[size] != '\0') ++size;
    return {text, size};
}

void register_host_window_class(const HINSTANCE instance) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = instance;
    window_class.lpszClassName = host_window_class_name;
    if (RegisterClassExW(&window_class) == 0
        && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        throw CaptureError(
            ErrorCode::backend_failure,
            "Could not register the ASIO host window: "
                + windows_error_message(HRESULT_FROM_WIN32(GetLastError())));
    }
}

} // namespace

struct AsioDriverSession::Impl final {
    HRESULT com_initialization = E_FAIL;
    HWND window = nullptr;
    IASIO* driver = nullptr;
    DWORD control_thread = 0;

    ~Impl() {
        if (driver != nullptr) {
            driver->Release();
            driver = nullptr;
        }
        if (window != nullptr) {
            DestroyWindow(window);
            window = nullptr;
        }
        if (com_initialization == S_OK || com_initialization == S_FALSE) {
            CoUninitialize();
        }
    }
};

AsioDriverSession::AsioDriverSession(const AsioDriverRegistration& registration)
    : impl_(std::make_unique<Impl>()) {
    impl_->control_thread = GetCurrentThreadId();
    impl_->com_initialization = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (impl_->com_initialization == RPC_E_CHANGED_MODE) {
        throw CaptureError(
            ErrorCode::backend_failure,
            "ASIO must run on a dedicated single-threaded COM apartment.");
    }
    if (FAILED(impl_->com_initialization)) {
        throw CaptureError(
            ErrorCode::backend_failure,
            "Could not initialize COM for ASIO: "
                + windows_error_message(impl_->com_initialization));
    }

    const auto instance = GetModuleHandleW(nullptr);
    register_host_window_class(instance);
    impl_->window = CreateWindowExW(
        0,
        host_window_class_name,
        L"Capture Panel ASIO Host",
        WS_OVERLAPPED,
        0,
        0,
        0,
        0,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (impl_->window == nullptr) {
        throw CaptureError(
            ErrorCode::backend_failure,
            "Could not create the ASIO host window: "
                + windows_error_message(HRESULT_FROM_WIN32(GetLastError())));
    }

    CLSID class_id{};
    const auto class_id_text = wide_ascii(registration.clsid);
    const auto parse_result = CLSIDFromString(class_id_text.c_str(), &class_id);
    if (FAILED(parse_result)) {
        throw CaptureError(
            ErrorCode::backend_failure,
            "The ASIO driver has an invalid CLSID: " + registration.clsid);
    }

    // The ASIO COM convention uses the driver's CLSID as both class and
    // requested interface ID; this matches Steinberg's reference host.
    const auto create_result = CoCreateInstance(
        class_id,
        nullptr,
        CLSCTX_INPROC_SERVER,
        class_id,
        reinterpret_cast<void**>(&impl_->driver));
    if (FAILED(create_result) || impl_->driver == nullptr) {
        throw CaptureError(
            ErrorCode::backend_failure,
            "Could not load ASIO driver '" + registration.name + "': "
                + windows_error_message(create_result));
    }

    if (impl_->driver->init(impl_->window) != ASIOTrue) {
        const auto detail = driver_error_message();
        throw CaptureError(
            ErrorCode::backend_failure,
            "Could not initialize ASIO driver '" + registration.name + "'"
                + (detail.empty() ? std::string{"."} : ": " + detail));
    }
}

AsioDriverSession::~AsioDriverSession() = default;

IASIO& AsioDriverSession::driver() const noexcept {
    return *impl_->driver;
}

std::string AsioDriverSession::driver_name() const {
    std::array<char, 32> name{};
    impl_->driver->getDriverName(name.data());
    return asio_text_to_utf8(bounded_asio_text(name.data(), name.size()));
}

std::string AsioDriverSession::driver_error_message() const {
    std::array<char, 124> message{};
    impl_->driver->getErrorMessage(message.data());
    auto result = asio_text_to_utf8(
        bounded_asio_text(message.data(), message.size()));
    while (!result.empty()
           && (result.back() == '\r' || result.back() == '\n'
               || result.back() == ' ' || result.back() == '\t')) {
        result.pop_back();
    }
    return result;
}

void AsioDriverSession::pump_messages() const noexcept {
    if (GetCurrentThreadId() != impl_->control_thread) return;
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
        if (message.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(message.wParam));
            return;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

bool asio_result_succeeded(const long result) noexcept {
    return result == ASE_OK || result == ASE_SUCCESS;
}

std::string asio_result_name(const long result) {
    switch (result) {
    case ASE_OK: return "ASE_OK";
    case ASE_SUCCESS: return "ASE_SUCCESS";
    case ASE_NotPresent: return "ASE_NotPresent";
    case ASE_HWMalfunction: return "ASE_HWMalfunction";
    case ASE_InvalidParameter: return "ASE_InvalidParameter";
    case ASE_InvalidMode: return "ASE_InvalidMode";
    case ASE_SPNotAdvancing: return "ASE_SPNotAdvancing";
    case ASE_NoClock: return "ASE_NoClock";
    case ASE_NoMemory: return "ASE_NoMemory";
    default: return "ASIO error " + std::to_string(result);
    }
}

std::string asio_text_to_utf8(const std::string_view value) {
    if (value.empty()) return {};
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return {};
    const auto length = static_cast<int>(value.size());
    const auto wide_length = MultiByteToWideChar(
        CP_ACP, MB_PRECOMPOSED, value.data(), length, nullptr, 0);
    if (wide_length <= 0) return std::string(value);
    std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
    if (MultiByteToWideChar(
            CP_ACP, MB_PRECOMPOSED, value.data(), length,
            wide.data(), wide_length) != wide_length) {
        return std::string(value);
    }
    return utf8_from_wide(wide);
}

} // namespace capture_panel::asio
