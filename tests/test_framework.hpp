#pragma once

#include <cmath>
#include <exception>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace capture_panel::test {

struct Case {
    std::string name;
    std::function<void()> body;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> body) {
        registry().push_back({std::move(name), std::move(body)});
    }
};

inline void require(bool condition, const char* expression, const char* file, int line) {
    if (condition) return;
    std::ostringstream message;
    message << file << ':' << line << ": requirement failed: " << expression;
    throw std::runtime_error(message.str());
}

inline void require_near(
    double actual,
    double expected,
    double tolerance,
    const char* file,
    int line) {
    if (std::abs(actual - expected) <= tolerance) return;
    std::ostringstream message;
    message << file << ':' << line << ": expected " << expected
            << " +/- " << tolerance << ", got " << actual;
    throw std::runtime_error(message.str());
}

} // namespace capture_panel::test

#define CP_TEST_JOIN_INNER(a, b) a##b
#define CP_TEST_JOIN(a, b) CP_TEST_JOIN_INNER(a, b)
#define CP_TEST_CASE(name) \
    static void CP_TEST_JOIN(cp_test_body_, __LINE__)(); \
    static ::capture_panel::test::Registrar CP_TEST_JOIN(cp_test_registrar_, __LINE__)( \
        name, CP_TEST_JOIN(cp_test_body_, __LINE__)); \
    static void CP_TEST_JOIN(cp_test_body_, __LINE__)()
#define CP_REQUIRE(expression) \
    ::capture_panel::test::require(static_cast<bool>(expression), #expression, __FILE__, __LINE__)
#define CP_REQUIRE_NEAR(actual, expected, tolerance) \
    ::capture_panel::test::require_near( \
        static_cast<double>(actual), static_cast<double>(expected), \
        static_cast<double>(tolerance), __FILE__, __LINE__)
