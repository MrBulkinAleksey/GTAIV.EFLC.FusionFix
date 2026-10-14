// A test runner small enough to read at once: TEST registers a function, CHECK records a failure and goes on.
// The executable returns the number of failed checks, so CI fails on any.
#pragma once
#include <cstdio>
#include <vector>

namespace check
{
    struct Test { const char* name; void (*run)(); };

    inline std::vector<Test>& Tests() { static std::vector<Test> tests; return tests; }
    inline int failures = 0;

    struct Register { Register(const char* name, void (*run)()) { Tests().push_back({ name, run }); } };

    inline void Fail(const char* file, int line, const char* expression)
    {
        std::printf("  FAILED %s:%d: %s\n", file, line, expression);
        ++failures;
    }

    inline int RunAll()
    {
        for (const auto& test : Tests())
        {
            const int before = failures;
            test.run();
            std::printf("%s %s\n", failures == before ? "ok    " : "FAILED", test.name);
        }
        std::printf("%zu tests, %d failed checks\n", Tests().size(), failures);
        return failures;
    }
}

#define CHECK_CAT2(a, b) a##b
#define CHECK_CAT(a, b) CHECK_CAT2(a, b)
#define TEST(name) \
    static void name(); \
    static check::Register CHECK_CAT(register_, name)(#name, &name); \
    static void name()
#define CHECK(expression) do { if (!(expression)) check::Fail(__FILE__, __LINE__, #expression); } while (0)
