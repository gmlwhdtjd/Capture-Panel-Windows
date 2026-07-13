#include "asio_temporary_float_file.hpp"

#include "capture_panel/core/errors.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace capture_panel::asio {
namespace {

std::atomic<std::uint64_t> unique_file_counter{0};

[[nodiscard]] std::filesystem::path default_scratch_prefix() {
    std::error_code error;
    auto directory = std::filesystem::temp_directory_path(error);
    if (error || directory.empty()) {
        throw CaptureError(
            ErrorCode::recording_write_failure,
            "Could not locate the temporary directory for an ASIO recording.");
    }
    return directory / L"CapturePanel.capture-panel.tmp.";
}

[[nodiscard]] std::wstring unique_suffix() {
    const auto counter = unique_file_counter.fetch_add(1, std::memory_order_relaxed);
    const auto ticks = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return std::to_wstring(GetCurrentProcessId()) + L"-"
        + std::to_wstring(ticks) + L"-" + std::to_wstring(counter) + L".f32";
}

[[nodiscard]] HANDLE create_unique_file(
    const std::filesystem::path& prefix,
    std::filesystem::path& created_path) {
    constexpr std::size_t maximum_attempts = 128;
    constexpr std::size_t maximum_windows_component_length = 255;
    for (std::size_t attempt = 0; attempt < maximum_attempts; ++attempt) {
        const auto suffix = unique_suffix();
        auto candidate_name = prefix.filename().native() + suffix;
        if (candidate_name.size() > maximum_windows_component_length) {
            // longPathAware removes MAX_PATH, but NTFS still limits each path
            // component. Keep a recognizable sibling fallback for unusually
            // long destination filenames.
            candidate_name = L".capture-panel.tmp." + suffix;
        }
        auto candidate = prefix.parent_path() / candidate_name;
        auto* handle = CreateFileW(
            candidate.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            created_path = std::move(candidate);
            return handle;
        }
        const auto error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            throw CaptureError(
                ErrorCode::recording_write_failure,
                "Could not create the temporary ASIO recording (Windows error "
                    + std::to_string(error) + ").");
        }
    }
    throw CaptureError(
        ErrorCode::recording_write_failure,
        "Could not allocate a unique temporary ASIO recording path.");
}

} // namespace

AsioTemporaryFloatFile::AsioTemporaryFloatFile(
    const std::optional<std::filesystem::path>& scratch_file_prefix) {
    const auto prefix = scratch_file_prefix.has_value()
        ? *scratch_file_prefix
        : default_scratch_prefix();
    if (prefix.empty()) {
        throw CaptureError(
            ErrorCode::recording_write_failure,
            "The temporary ASIO recording prefix is empty.");
    }
    handle_ = create_unique_file(prefix, path_);
}

AsioTemporaryFloatFile::~AsioTemporaryFloatFile() {
    if (handle_ != nullptr) {
        static_cast<void>(CloseHandle(static_cast<HANDLE>(handle_)));
        handle_ = nullptr;
    }
    if (owns_path_ && !path_.empty()) {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
}

void AsioTemporaryFloatFile::write(const std::span<const float> samples) {
    if (closed_ || handle_ == nullptr) {
        throw CaptureError(
            ErrorCode::recording_write_failure,
            "The temporary ASIO recording is already closed.");
    }

    const auto* bytes = reinterpret_cast<const std::byte*>(samples.data());
    auto remaining = samples.size_bytes();
    while (remaining > 0) {
        const auto request = static_cast<DWORD>(std::min<std::size_t>(
            remaining,
            std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(
                static_cast<HANDLE>(handle_),
                bytes,
                request,
                &written,
                nullptr)
            || written == 0) {
            const auto error = written == 0 ? ERROR_WRITE_FAULT : GetLastError();
            throw CaptureError(
                ErrorCode::recording_write_failure,
                "Could not write the temporary ASIO recording (Windows error "
                    + std::to_string(error) + ").");
        }
        bytes += written;
        remaining -= written;
    }
}

void AsioTemporaryFloatFile::close() {
    if (closed_) return;
    if (handle_ == nullptr) {
        throw CaptureError(
            ErrorCode::recording_write_failure,
            "The temporary ASIO recording has no open file handle.");
    }
    if (!FlushFileBuffers(static_cast<HANDLE>(handle_))) {
        const auto error = GetLastError();
        throw CaptureError(
            ErrorCode::recording_write_failure,
            "Could not flush the temporary ASIO recording (Windows error "
                + std::to_string(error) + ").");
    }
    if (!CloseHandle(static_cast<HANDLE>(handle_))) {
        const auto error = GetLastError();
        handle_ = nullptr;
        throw CaptureError(
            ErrorCode::recording_write_failure,
            "Could not close the temporary ASIO recording (Windows error "
                + std::to_string(error) + ").");
    }
    handle_ = nullptr;
    closed_ = true;
}

std::filesystem::path AsioTemporaryFloatFile::release_ownership() {
    if (!closed_ || handle_ != nullptr) {
        throw CaptureError(
            ErrorCode::recording_write_failure,
            "The temporary ASIO recording must be closed before ownership transfer.");
    }
    owns_path_ = false;
    return path_;
}

} // namespace capture_panel::asio
