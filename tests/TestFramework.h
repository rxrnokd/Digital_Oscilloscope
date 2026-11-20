// Minimal dependency-free test framework. Every TEST() is registered by name and can be
// run individually (ctest runs each one as a separate test case).
#pragma once

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace slptest {

struct Case {
    const char* name;
    std::function<void()> fn;
};
inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
inline int& failures() {
    static int f = 0;
    return f;
}
struct Reg {
    Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};

inline int runAll(int argc, char** argv) {
    int ran = 0;
    for (auto& c : registry()) {
        if (argc > 1 && std::strcmp(argv[1], c.name) != 0) continue;
        const int before = failures();
        std::printf("[ RUN  ] %s\n", c.name);
        c.fn();
        std::printf("[ %s ] %s\n", failures() == before ? " OK " : "FAIL", c.name);
        ++ran;
    }
    if (ran == 0) {
        std::printf("no test matched '%s'. available:\n", argc > 1 ? argv[1] : "");
        for (auto& c : registry()) std::printf("  %s\n", c.name);
        return 2;
    }
    std::printf("%d test(s), %d failure(s)\n", ran, failures());
    return failures() ? 1 : 0;
}

} // namespace slptest

#define SLP_CAT2(a, b) a##b
#define SLP_CAT(a, b) SLP_CAT2(a, b)
#define TEST(name)                                                                  \
    static void SLP_CAT(test_, name)();                                             \
    static slptest::Reg SLP_CAT(reg_, name)(#name, &SLP_CAT(test_, name));          \
    static void SLP_CAT(test_, name)()

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::printf("  %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);  \
            ++slptest::failures();                                                  \
        }                                                                           \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                       \
    do {                                                                            \
        const double va_ = (a), vb_ = (b), vt_ = (tol);                             \
        if (!(std::fabs(va_ - vb_) <= vt_)) {                                       \
            std::printf("  %s:%d: CHECK_NEAR failed: %s = %.10g, expected %.10g +- %.3g\n", \
                        __FILE__, __LINE__, #a, va_, vb_, vt_);                     \
            ++slptest::failures();                                                  \
        }                                                                           \
    } while (0)
