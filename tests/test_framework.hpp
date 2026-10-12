// A tiny self-registering test framework, so the project builds with no
// third-party downloads. Usage:
//
//   TEST(lru_evicts_oldest) {
//       CHECK_EQ(1 + 1, 2);
//       CHECK_THROWS(parse("bad"));
//   }
//
// Run all tests:          ./cachesim_tests
// Run matching tests:     ./cachesim_tests lru
#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace testing {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

struct State {
    int failures = 0;      // failed checks in the current test
    long checks = 0;       // total checks executed
};

inline State& state() {
    static State s;
    return s;
}

template <typename T>
std::string show(const T& v) {
    std::ostringstream os;
    if constexpr (std::is_same_v<T, std::uint8_t> || std::is_same_v<T, std::int8_t>) os << static_cast<int>(v);
    else if constexpr (std::is_enum_v<T>) os << "enum(" << static_cast<long long>(v) << ")";
    else if constexpr (std::is_same_v<T, bool>) os << (v ? "true" : "false");
    else os << v;
    return os.str();
}

inline void report_failure(const char* file, int line, const std::string& what) {
    ++state().failures;
    std::cerr << "    " << file << ":" << line << ": " << what << "\n";
}

struct FatalFailure {};

}  // namespace testing

#define TEST_CONCAT2(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT2(a, b)

#define TEST(name)                                                                      \
    static void TEST_CONCAT(test_fn_, name)();                                          \
    static const ::testing::Registrar TEST_CONCAT(test_reg_, name)(#name, TEST_CONCAT(test_fn_, name)); \
    static void TEST_CONCAT(test_fn_, name)()

#define CHECK(cond)                                                                     \
    do {                                                                                \
        ++::testing::state().checks;                                                    \
        if (!(cond)) ::testing::report_failure(__FILE__, __LINE__, "CHECK(" #cond ") failed"); \
    } while (0)

#define REQUIRE(cond)                                                                   \
    do {                                                                                \
        ++::testing::state().checks;                                                    \
        if (!(cond)) {                                                                  \
            ::testing::report_failure(__FILE__, __LINE__, "REQUIRE(" #cond ") failed"); \
            throw ::testing::FatalFailure{};                                            \
        }                                                                               \
    } while (0)

#define CHECK_EQ(a, b)                                                                  \
    do {                                                                                \
        ++::testing::state().checks;                                                    \
        const auto va_ = (a);                                                           \
        const auto vb_ = (b);                                                           \
        if (!(va_ == vb_)) {                                                            \
            ::testing::report_failure(__FILE__, __LINE__,                               \
                "CHECK_EQ(" #a ", " #b "): " + ::testing::show(va_) + " != " + ::testing::show(vb_)); \
        }                                                                               \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                           \
    do {                                                                                \
        ++::testing::state().checks;                                                    \
        const double va_ = (a), vb_ = (b);                                              \
        if (!(va_ - vb_ <= (eps) && vb_ - va_ <= (eps))) {                              \
            ::testing::report_failure(__FILE__, __LINE__,                               \
                "CHECK_NEAR(" #a ", " #b "): " + ::testing::show(va_) + " vs " + ::testing::show(vb_)); \
        }                                                                               \
    } while (0)

#define CHECK_THROWS(expr)                                                              \
    do {                                                                                \
        ++::testing::state().checks;                                                    \
        bool threw_ = false;                                                            \
        try { (void)(expr); } catch (...) { threw_ = true; }                            \
        if (!threw_) ::testing::report_failure(__FILE__, __LINE__, "CHECK_THROWS(" #expr ") did not throw"); \
    } while (0)

#define CHECK_THROWS_WITH(expr, substring)                                              \
    do {                                                                                \
        ++::testing::state().checks;                                                    \
        std::string msg_;                                                               \
        bool threw_ = false;                                                            \
        try { (void)(expr); } catch (const std::exception& e_) { threw_ = true; msg_ = e_.what(); } \
        if (!threw_) ::testing::report_failure(__FILE__, __LINE__, "CHECK_THROWS_WITH(" #expr ") did not throw"); \
        else if (msg_.find(substring) == std::string::npos)                             \
            ::testing::report_failure(__FILE__, __LINE__, "exception message '" + msg_ + "' lacks '" + std::string(substring) + "'"); \
    } while (0)
