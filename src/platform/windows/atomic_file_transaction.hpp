#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

namespace capture_panel::platform {

class AtomicFileTransactionError final : public std::runtime_error {
public:
    explicit AtomicFileTransactionError(std::string detail)
        : std::runtime_error(std::move(detail)) {}
};

// Owns a uniquely reserved sibling file until it is atomically promoted to
// the destination. The declaration intentionally contains no Windows types so
// platform-independent writers can use the transaction boundary without
// importing Win32 headers.
class AtomicFileTransaction final {
public:
    explicit AtomicFileTransaction(const std::filesystem::path& destination);

    AtomicFileTransaction(const AtomicFileTransaction&) = delete;
    AtomicFileTransaction& operator=(const AtomicFileTransaction&) = delete;
    AtomicFileTransaction(AtomicFileTransaction&&) = delete;
    AtomicFileTransaction& operator=(AtomicFileTransaction&&) = delete;

    ~AtomicFileTransaction() noexcept;

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    void promote_to(const std::filesystem::path& destination);

private:
    std::filesystem::path path_;
    bool promoted_ = false;
};

} // namespace capture_panel::platform
