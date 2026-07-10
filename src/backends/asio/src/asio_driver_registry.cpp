#include "asio_driver_registry.hpp"

#include <algorithm>
#include <array>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace capture_panel::asio {
namespace {

[[nodiscard]] constexpr bool is_ascii_space(const char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
           value == '\f' || value == '\v';
}

[[nodiscard]] std::string_view trim_ascii(std::string_view value) noexcept {
    while (!value.empty() && is_ascii_space(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_ascii_space(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] constexpr bool is_hex_digit(const char value) noexcept {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

[[nodiscard]] constexpr char uppercase_hex(const char value) noexcept {
    return value >= 'a' && value <= 'f'
        ? static_cast<char>(value - ('a' - 'A'))
        : value;
}

#if defined(_WIN32)

class RegistryKey final {
public:
    RegistryKey() = default;
    explicit RegistryKey(HKEY key) noexcept : key_(key) {}

    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;

    RegistryKey(RegistryKey&& other) noexcept : key_(std::exchange(other.key_, nullptr)) {}

    RegistryKey& operator=(RegistryKey&& other) noexcept {
        if (this != &other) {
            reset();
            key_ = std::exchange(other.key_, nullptr);
        }
        return *this;
    }

    ~RegistryKey() { reset(); }

    [[nodiscard]] HKEY get() const noexcept { return key_; }
    [[nodiscard]] explicit operator bool() const noexcept { return key_ != nullptr; }

private:
    void reset() noexcept {
        if (key_ != nullptr) {
            static_cast<void>(RegCloseKey(key_));
            key_ = nullptr;
        }
    }

    HKEY key_ = nullptr;
};

[[nodiscard]] RegistryKey open_machine_key(const std::wstring& path) noexcept {
    HKEY raw_key = nullptr;
    const auto status = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        path.c_str(),
        0,
        KEY_READ | KEY_WOW64_64KEY,
        &raw_key);
    return status == ERROR_SUCCESS ? RegistryKey(raw_key) : RegistryKey{};
}

[[nodiscard]] RegistryKey open_subkey(HKEY parent, const std::wstring& name) noexcept {
    HKEY raw_key = nullptr;
    const auto status = RegOpenKeyExW(
        parent,
        name.c_str(),
        0,
        KEY_READ | KEY_WOW64_64KEY,
        &raw_key);
    return status == ERROR_SUCCESS ? RegistryKey(raw_key) : RegistryKey{};
}

[[nodiscard]] std::optional<std::wstring> read_string_value(
    HKEY key,
    const wchar_t* value_name) {
    DWORD type = 0;
    DWORD byte_count = 0;
    auto status = RegQueryValueExW(key, value_name, nullptr, &type, nullptr, &byte_count);
    if (status != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) ||
        byte_count == 0) {
        return std::nullopt;
    }

    // Registry strings are not guaranteed to include a terminating NUL.
    std::vector<wchar_t> buffer(
        static_cast<std::size_t>(byte_count) / sizeof(wchar_t) + 1,
        L'\0');
    status = RegQueryValueExW(
        key,
        value_name,
        nullptr,
        &type,
        reinterpret_cast<BYTE*>(buffer.data()),
        &byte_count);
    if (status != ERROR_SUCCESS) {
        return std::nullopt;
    }

    const auto terminator = std::find(buffer.cbegin(), buffer.cend(), L'\0');
    std::wstring result(buffer.cbegin(), terminator);
    if (type != REG_EXPAND_SZ || result.empty()) {
        return result;
    }

    const auto required = ExpandEnvironmentStringsW(result.c_str(), nullptr, 0);
    if (required == 0) {
        return result;
    }
    std::vector<wchar_t> expanded(static_cast<std::size_t>(required), L'\0');
    const auto written = ExpandEnvironmentStringsW(
        result.c_str(), expanded.data(), required);
    if (written == 0 || written > required) {
        return result;
    }
    return std::wstring(expanded.data(), static_cast<std::size_t>(written - 1));
}

[[nodiscard]] std::string utf8_from_wide(const std::wstring_view value) {
    if (value.empty()) {
        return {};
    }

    const auto input_size = static_cast<int>(value.size());
    const auto required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), input_size,
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    const auto written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), input_size,
        result.data(), required, nullptr, nullptr);
    return written == required ? result : std::string{};
}

[[nodiscard]] std::wstring wide_from_ascii(const std::string_view value) {
    return std::wstring(value.begin(), value.end());
}

[[nodiscard]] std::filesystem::path registered_driver_path(
    HKEY asio_driver_key,
    const std::string_view canonical_clsid) {
    if (const auto explicit_driver = read_string_value(asio_driver_key, L"Driver");
        explicit_driver && !explicit_driver->empty()) {
        return std::filesystem::path(*explicit_driver);
    }

    const std::wstring class_path =
        L"SOFTWARE\\Classes\\CLSID\\" + wide_from_ascii(canonical_clsid) +
        L"\\InprocServer32";
    const auto class_key = open_machine_key(class_path);
    if (!class_key) {
        return {};
    }
    const auto server_path = read_string_value(class_key.get(), nullptr);
    return server_path ? std::filesystem::path(*server_path) : std::filesystem::path{};
}

[[nodiscard]] std::vector<std::wstring> enumerate_subkey_names(HKEY key) {
    DWORD subkey_count = 0;
    DWORD maximum_name_length = 0;
    const auto status = RegQueryInfoKeyW(
        key,
        nullptr,
        nullptr,
        nullptr,
        &subkey_count,
        &maximum_name_length,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr);
    if (status != ERROR_SUCCESS) {
        return {};
    }

    std::vector<std::wstring> names;
    names.reserve(static_cast<std::size_t>(subkey_count));
    std::vector<wchar_t> name_buffer(
        static_cast<std::size_t>(maximum_name_length) + 1,
        L'\0');
    for (DWORD index = 0; index < subkey_count; ++index) {
        DWORD name_length = maximum_name_length + 1;
        if (RegEnumKeyExW(
                key,
                index,
                name_buffer.data(),
                &name_length,
                nullptr,
                nullptr,
                nullptr,
                nullptr) == ERROR_SUCCESS) {
            names.emplace_back(name_buffer.data(), static_cast<std::size_t>(name_length));
        }
    }
    return names;
}

#endif

} // namespace

std::optional<std::string> canonicalize_clsid(std::string_view value) {
    value = trim_ascii(value);
    if (value.size() == 38 && value.front() == '{' && value.back() == '}') {
        value.remove_prefix(1);
        value.remove_suffix(1);
    }
    if (value.size() != 36) {
        return std::nullopt;
    }

    constexpr std::array<std::size_t, 4> hyphen_positions{8, 13, 18, 23};
    std::string result;
    result.reserve(38);
    result.push_back('{');
    for (std::size_t index = 0; index < value.size(); ++index) {
        const bool is_hyphen_position = std::find(
            hyphen_positions.begin(), hyphen_positions.end(), index) !=
            hyphen_positions.end();
        if (is_hyphen_position) {
            if (value[index] != '-') {
                return std::nullopt;
            }
            result.push_back('-');
            continue;
        }
        if (!is_hex_digit(value[index])) {
            return std::nullopt;
        }
        result.push_back(uppercase_hex(value[index]));
    }
    result.push_back('}');
    return result;
}

std::optional<std::string> make_asio_device_id(const std::string_view clsid) {
    auto canonical = canonicalize_clsid(clsid);
    if (!canonical) {
        return std::nullopt;
    }
    return std::string("asio:") + *canonical;
}

std::optional<std::string> parse_asio_device_id(const std::string_view device_id) {
    constexpr std::string_view prefix = "asio:";
    if (!device_id.starts_with(prefix)) {
        return std::nullopt;
    }
    return canonicalize_clsid(device_id.substr(prefix.size()));
}

std::vector<AsioDriverRegistration> enumerate_asio_drivers() {
#if defined(_WIN32)
    const auto asio_root = open_machine_key(L"SOFTWARE\\ASIO");
    if (!asio_root) {
        return {};
    }

    std::vector<AsioDriverRegistration> drivers;
    for (const auto& subkey_name : enumerate_subkey_names(asio_root.get())) {
        const auto driver_key = open_subkey(asio_root.get(), subkey_name);
        if (!driver_key) {
            continue;
        }

        const auto registered_clsid = read_string_value(driver_key.get(), L"CLSID");
        if (!registered_clsid) {
            continue;
        }
        const auto clsid = canonicalize_clsid(utf8_from_wide(*registered_clsid));
        if (!clsid) {
            continue;
        }

        auto name = utf8_from_wide(subkey_name);
        const auto registered_description = read_string_value(driver_key.get(), L"Description");
        auto description = registered_description
            ? utf8_from_wide(*registered_description)
            : std::string{};
        if (description.empty()) {
            description = name;
        }

        drivers.push_back({
            .name = std::move(name),
            .description = std::move(description),
            .clsid = *clsid,
            .id = std::string("asio:") + *clsid,
            .driver_path = registered_driver_path(driver_key.get(), *clsid),
        });
    }

    std::ranges::sort(drivers, {}, &AsioDriverRegistration::id);
    return drivers;
#else
    return {};
#endif
}

} // namespace capture_panel::asio
