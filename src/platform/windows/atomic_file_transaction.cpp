#include "atomic_file_transaction.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace capture_panel::platform {
namespace {

std::atomic_uint64_t temporary_file_sequence{0};
constexpr std::size_t maximum_windows_component_length = 255;

[[nodiscard]] std::string windows_error_detail(const DWORD error) {
    return "Windows error " + std::to_string(static_cast<unsigned long>(error));
}

[[nodiscard]] std::filesystem::path reserve_sibling_temporary_file(
    const std::filesystem::path& destination) {
    constexpr std::uint64_t maximum_attempts = 128;
    for (std::uint64_t attempt = 0; attempt < maximum_attempts; ++attempt) {
        const auto sequence = temporary_file_sequence.fetch_add(
            1U, std::memory_order_relaxed);
        auto suffix = std::wstring(L".capture-panel.tmp.");
        suffix += std::to_wstring(GetCurrentProcessId());
        suffix += L'.';
        suffix += std::to_wstring(sequence);

        auto candidate_name = destination.filename().native();
        candidate_name += suffix;
        if (candidate_name.size() > maximum_windows_component_length) {
            // longPathAware removes the overall MAX_PATH limit, not the NTFS
            // per-component limit. Keep the destination-derived name whenever
            // it fits, but use a short sibling for long destination names.
            candidate_name = L".capture-panel.tmp.";
            candidate_name += std::to_wstring(GetCurrentProcessId());
            candidate_name += L'.';
            candidate_name += std::to_wstring(sequence);
        }
        const auto candidate = destination.parent_path() / candidate_name;

        const auto handle = CreateFileW(
            candidate.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY,
            nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            if (CloseHandle(handle) == FALSE) {
                const auto error = GetLastError();
                static_cast<void>(DeleteFileW(candidate.c_str()));
                throw AtomicFileTransactionError(
                    "could not reserve a temporary output file ("
                    + windows_error_detail(error) + ')');
            }
            return candidate;
        }

        const auto error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            throw AtomicFileTransactionError(
                "could not reserve a sibling temporary output file ("
                + windows_error_detail(error) + ')');
        }
    }
    throw AtomicFileTransactionError(
        "could not reserve a unique temporary output file");
}

} // namespace

AtomicFileTransaction::AtomicFileTransaction(const std::filesystem::path& destination)
    : path_(reserve_sibling_temporary_file(destination)) {}

AtomicFileTransaction::~AtomicFileTransaction() noexcept {
    if (!promoted_) static_cast<void>(DeleteFileW(path_.c_str()));
}

const std::filesystem::path& AtomicFileTransaction::path() const noexcept {
    return path_;
}

void AtomicFileTransaction::promote_to(const std::filesystem::path& destination) {
    if (MoveFileExW(
            path_.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        const auto error = GetLastError();
        throw AtomicFileTransactionError(
            "could not atomically replace the output file ("
            + windows_error_detail(error) + ')');
    }
    promoted_ = true;
}

} // namespace capture_panel::platform
