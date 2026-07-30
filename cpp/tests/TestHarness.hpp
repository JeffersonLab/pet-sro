// TestHarness.hpp -- a ~100 line test runner.
//
// Deliberately not GoogleTest: the project's stated rule is not to add large
// third-party dependencies silently, and what these tests need is a way to
// register a function, compare values, and report the first failure with a
// file and line. That is all this does.

#ifndef PETSRO_TESTHARNESS_HPP
#define PETSRO_TESTHARNESS_HPP

#include <cstdint>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace petsro {
namespace test {

struct TestCase {
    std::string name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

/// Thrown by the assertion macros; caught by the runner.
struct AssertionFailure {
    std::string message;
};

struct Registrar {
    Registrar(const char* name, std::function<void()> body) {
        registry().push_back(TestCase{name, std::move(body)});
    }
};

inline int runAll() {
    int failed = 0;
    for (const TestCase& tc : registry()) {
        try {
            tc.body();
            std::cout << "  PASS  " << tc.name << '\n';
        } catch (const AssertionFailure& f) {
            std::cout << "  FAIL  " << tc.name << '\n' << f.message << '\n';
            ++failed;
        } catch (const std::exception& e) {
            std::cout << "  FAIL  " << tc.name << "\n        unexpected exception: " << e.what()
                      << '\n';
            ++failed;
        }
    }

    const std::size_t total = registry().size();
    std::cout << "\n" << (total - static_cast<std::size_t>(failed)) << '/' << total
              << " test(s) passed\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace test
}  // namespace petsro

#define PETSRO_CAT_(a, b) a##b
#define PETSRO_CAT(a, b) PETSRO_CAT_(a, b)

/// Defines and registers a test. Use as: TEST(name) { ... }
#define TEST(name)                                                              \
    static void PETSRO_CAT(petsro_test_, name)();                               \
    static ::petsro::test::Registrar PETSRO_CAT(petsro_reg_, name)(             \
        #name, &PETSRO_CAT(petsro_test_, name));                                \
    static void PETSRO_CAT(petsro_test_, name)()

#define PETSRO_FAIL(msg)                                                        \
    do {                                                                        \
        std::ostringstream petsro_oss_;                                         \
        petsro_oss_ << "        at " << __FILE__ << ':' << __LINE__ << "\n        " \
                    << (msg);                                                   \
        throw ::petsro::test::AssertionFailure{petsro_oss_.str()};              \
    } while (false)

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            PETSRO_FAIL("expected true: " #cond);                               \
        }                                                                       \
    } while (false)

#define CHECK_FALSE(cond)                                                       \
    do {                                                                        \
        if ((cond)) {                                                           \
            PETSRO_FAIL("expected false: " #cond);                              \
        }                                                                       \
    } while (false)

#define CHECK_EQ(actual, expected)                                              \
    do {                                                                        \
        const auto petsro_a_ = (actual);                                        \
        const auto petsro_e_ = (expected);                                      \
        if (!(petsro_a_ == petsro_e_)) {                                        \
            std::ostringstream petsro_m_;                                       \
            petsro_m_ << #actual " == " #expected "\n          actual   : "     \
                      << petsro_a_ << "\n          expected : " << petsro_e_;   \
            PETSRO_FAIL(petsro_m_.str());                                       \
        }                                                                       \
    } while (false)

#endif  // PETSRO_TESTHARNESS_HPP
