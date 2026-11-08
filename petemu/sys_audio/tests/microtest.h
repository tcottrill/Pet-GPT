// Minimal header-only test harness. No deps.
#pragma once
#include <cstdio>
#include <cstdint>
static int g_tests = 0, g_fails = 0;
#define CHECK(cond) do { \
    g_tests++; \
    if (!(cond)) { g_fails++; \
        std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while(0)
#define CHECK_EQ(a,b) do { \
    g_tests++; \
    auto _a=(a); auto _b=(b); \
    if (!((_a)==(_b))) { g_fails++; \
        std::printf("FAIL %s:%d  %s==%s  (got %lld vs %lld)\n", \
        __FILE__, __LINE__, #a, #b, (long long)(_a), (long long)(_b)); } \
} while(0)
#define TEST_MAIN() int main(){ run_all(); \
    std::printf("\n%d checks, %d failures\n", g_tests, g_fails); \
    return g_fails ? 1 : 0; }
