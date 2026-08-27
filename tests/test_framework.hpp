#pragma once
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

// A tiny dependency-free test framework. It is intentionally minimal: the point
// of these tests is to exercise real runtime behavior, not framework features.

namespace df_test {

using Fn = void (*)();

struct Case {
  const char* name;
  Fn fn;
};

inline std::vector<Case>& registry() {
  static std::vector<Case> r;
  return r;
}

struct Registrar {
  Registrar(const char* name, Fn fn) { registry().push_back(Case{name, fn}); }
};

inline int& failure_count() { static int n = 0; return n; }
inline int& check_count() { static int n = 0; return n; }

inline void report_fail(const char* file, int line, const char* expr) {
  ++failure_count();
  std::printf("  [FAIL] %s:%d  %s\n", file, line, expr);
}

inline int run_all(int argc, char** argv) {
  const char* filter = nullptr;
  if (argc > 1) filter = argv[1];
  int ran = 0;
  for (const auto& c : registry()) {
    if (filter && std::strstr(c.name, filter) == nullptr) continue;
    int before = check_count();
    int failures_before = failure_count();
    std::printf("[RUN ] %s\n", c.name);
    try {
      c.fn();
    } catch (const std::exception& ex) {
      ++failure_count();
      std::printf("  [EXCEPTION] %s\n", ex.what());
    } catch (...) {
      ++failure_count();
      std::printf("  [EXCEPTION] unknown\n");
    }
    ++ran;
    if (failure_count() == failures_before) {
      std::printf("[ OK ] %s  (%d checks)\n", c.name, check_count() - before);
    }
  }
  std::printf("\n%d tests run, %d failures\n", ran, failure_count());
  return failure_count() == 0 ? 0 : 1;
}

}  // namespace df_test

namespace df_test {
inline void check_true(bool ok, const char* file, int line, const char* expr) {
  ++check_count();
  if (!ok) report_fail(file, line, expr);
}
inline void check_true_impl(const char* a, const char* b, bool ok, const char* file, int line) {
  ++check_count();
  if (!ok) report_fail(file, line, a);
  (void)b;
}
}  // namespace df_test

#define DF_TEST(name)                                                        \
  static void df_test_func_##name();                                         \
  static ::df_test::Registrar df_test_reg_##name(#name, &df_test_func_##name); \
  static void df_test_func_##name()

#define CHECK(expr)                                                          \
  ::df_test::check_true(!!(expr), __FILE__, __LINE__, #expr)

#define CHECK_EQ(a, b)                                                       \
  ::df_test::check_true(((a) == (b)), __FILE__, __LINE__, #a " != " #b)
