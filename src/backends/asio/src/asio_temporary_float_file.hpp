#pragma once

#include <filesystem>
#include <optional>
#include <span>

namespace capture_panel::asio {

/// A uniquely-created raw interleaved Float32 scratch file.
///
/// The file is deleted on destruction unless ownership is explicitly released
/// after a successful close. This keeps every pre-start and worker failure
/// path exception-safe while allowing Float32AudioAsset to take over cleanup.
class AsioTemporaryFloatFile final {
public:
    explicit AsioTemporaryFloatFile(
        const std::optional<std::filesystem::path>& scratch_file_prefix);
    ~AsioTemporaryFloatFile();

    AsioTemporaryFloatFile(const AsioTemporaryFloatFile&) = delete;
    AsioTemporaryFloatFile& operator=(const AsioTemporaryFloatFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    void write(std::span<const float> samples);
    void close();

    /// Disarms destructor deletion. The caller must first close the file and
    /// immediately transfer the returned path to another RAII owner.
    [[nodiscard]] std::filesystem::path release_ownership();

private:
    std::filesystem::path path_;
    void* handle_ = nullptr;
    bool closed_ = false;
    bool owns_path_ = true;
};

} // namespace capture_panel::asio
