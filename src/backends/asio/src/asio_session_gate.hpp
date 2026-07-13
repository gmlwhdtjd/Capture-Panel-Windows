#pragma once

#include "capture_panel/core/errors.hpp"

#include <mutex>

namespace capture_panel::asio {

// Vendor ASIO drivers are in-process COM servers and are commonly exclusive.
// Serialize every driver-touching public operation, not only createBuffers, so
// a probe or sample-rate change cannot race a running capture.
[[nodiscard]] inline std::mutex& process_asio_session_mutex() noexcept {
    static std::mutex mutex;
    return mutex;
}

[[nodiscard]] inline bool& current_thread_owns_asio_session() noexcept {
    thread_local bool owns_session = false;
    return owns_session;
}

class AsioSessionLease final {
public:
    AsioSessionLease()
        : lock_([] {
              if (current_thread_owns_asio_session()) {
                  throw CaptureError(
                      ErrorCode::backend_failure,
                      "An ASIO control operation cannot be re-entered.");
              }
              return std::unique_lock<std::mutex>(process_asio_session_mutex());
          }()) {
        current_thread_owns_asio_session() = true;
    }

    AsioSessionLease(const AsioSessionLease&) = delete;
    AsioSessionLease& operator=(const AsioSessionLease&) = delete;
    AsioSessionLease(AsioSessionLease&&) = delete;
    AsioSessionLease& operator=(AsioSessionLease&&) = delete;

    ~AsioSessionLease() { unlock(); }

    void unlock() noexcept {
        if (!lock_.owns_lock()) return;
        current_thread_owns_asio_session() = false;
        lock_.unlock();
    }

private:
    std::unique_lock<std::mutex> lock_;
};

[[nodiscard]] inline AsioSessionLease acquire_asio_session() {
    return AsioSessionLease();
}

} // namespace capture_panel::asio
