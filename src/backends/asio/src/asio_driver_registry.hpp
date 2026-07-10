#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace capture_panel::asio {

// A single 64-bit ASIO driver registration discovered below
// HKLM\SOFTWARE\ASIO. Text fields are UTF-8; driver_path retains the native
// filesystem representation so non-ASCII install paths remain lossless.
struct AsioDriverRegistration {
    std::string name;
    std::string description;
    std::string clsid;
    std::string id;
    std::filesystem::path driver_path;
};

// Accepts the registry's usual braced GUID form as well as an unbraced GUID.
// The result always uses uppercase hexadecimal and surrounding braces.
[[nodiscard]] std::optional<std::string> canonicalize_clsid(std::string_view value);

// Returns "asio:{CANONICAL-GUID}" or nullopt when clsid is malformed.
[[nodiscard]] std::optional<std::string> make_asio_device_id(std::string_view clsid);

// Parses an ASIO device id and returns its canonical braced CLSID.
[[nodiscard]] std::optional<std::string> parse_asio_device_id(std::string_view device_id);

// Invalid or incomplete CLSID registrations are ignored. A valid registration
// remains visible when its COM server path cannot be resolved; driver_path is
// empty in that case so callers can report it as unavailable without losing the
// registry identity.
[[nodiscard]] std::vector<AsioDriverRegistration> enumerate_asio_drivers();

} // namespace capture_panel::asio
