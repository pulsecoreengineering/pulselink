#ifndef PULSELINK_TEST_PL_TEST_H
#define PULSELINK_TEST_PL_TEST_H

// Minimal, dependency-free host-native test harness. Deliberately not part
// of core/ or transport/, so it is not bound by the zero-heap contract —
// std::string/std::vector/std::function are fine here.

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace pltest {

struct Case {
  std::string name;
  std::function<void()> fn;
};

inline std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

inline int& failure_count() {
  static int n = 0;
  return n;
}

struct Registrar {
  Registrar(const std::string& name, const std::function<void()>& fn) {
    registry().push_back(Case{name, fn});
  }
};

inline void report_failure(const char* expr, const char* file, int line) {
  std::fprintf(stderr, "  FAIL: %s (%s:%d)\n", expr, file, line);
  ++failure_count();
}

inline int run_all() {
  int total = 0;
  for (auto& c : registry()) {
    int before = failure_count();
    c.fn();
    ++total;
    std::printf("[%s] %s\n", failure_count() == before ? " OK " : "FAIL",
                c.name.c_str());
  }
  std::printf("%d test(s), %d failure(s)\n", total, failure_count());
  return failure_count() == 0 ? 0 : 1;
}

}  // namespace pltest

#define PL_TEST_CASE(name)                                             \
  static void name();                                                  \
  static pltest::Registrar registrar_##name(#name, name); /* NOLINT */ \
  static void name()

#define PL_ASSERT(cond)                                     \
  do {                                                       \
    if (!(cond)) {                                           \
      pltest::report_failure(#cond, __FILE__, __LINE__);     \
    }                                                        \
  } while (0)

#endif  // PULSELINK_TEST_PL_TEST_H
