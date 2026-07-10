#include "test_framework.hpp"

#include <iostream>

int main() {
    int failures = 0;
    for (const auto& test : capture_panel::test::registry()) {
        try {
            test.body();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
        }
    }

    std::cout << capture_panel::test::registry().size() << " test(s), "
              << failures << " failure(s)\n";
    return failures == 0 ? 0 : 1;
}
