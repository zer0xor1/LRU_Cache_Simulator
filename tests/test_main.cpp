#include <chrono>
#include <cstring>

#include "test_framework.hpp"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int passed = 0, failed = 0, skipped = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (const auto& tc : testing::registry()) {
        if (filter && !std::strstr(tc.name, filter)) {
            ++skipped;
            continue;
        }
        testing::state().failures = 0;
        bool crashed = false;
        try {
            tc.fn();
        } catch (const testing::FatalFailure&) {
        } catch (const std::exception& e) {
            crashed = true;
            std::cerr << "    unexpected exception: " << e.what() << "\n";
        }
        if (testing::state().failures || crashed) {
            ++failed;
            std::cerr << "[FAIL] " << tc.name << "\n";
        } else {
            ++passed;
            std::cout << "[ ok ] " << tc.name << "\n";
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "\n" << passed << " passed, " << failed << " failed";
    if (skipped) std::cout << ", " << skipped << " filtered out";
    std::cout << " (" << testing::state().checks << " checks, " << static_cast<long>(ms) << " ms)\n";
    return failed ? 1 : 0;
}
