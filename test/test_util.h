// Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
//
// Small helpers shared by the host test programs.

#ifndef VOX_TEST_TEST_UTIL_H_
#define VOX_TEST_TEST_UTIL_H_

#include <math.h>
#include <stdarg.h>
#include <stdio.h>

#include <algorithm>
#include <vector>

namespace vox_test {

inline int& FailureCount() { static int n = 0; return n; }
inline int& CheckCount() { static int n = 0; return n; }

inline void Check(bool ok, const char* name, const char* fmt, ...) {
  ++CheckCount();
  char detail[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(detail, sizeof(detail), fmt, args);
  va_end(args);
  printf("  [%s] %-50s %s\n", ok ? "PASS" : "FAIL", name, detail);
  if (!ok) ++FailureCount();
}

inline void Info(const char* name, const char* fmt, ...) {
  char detail[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(detail, sizeof(detail), fmt, args);
  va_end(args);
  printf("  [INFO] %-50s %s\n", name, detail);
}

inline int Summary() {
  printf("\n%d/%d checks passed\n", CheckCount() - FailureCount(), CheckCount());
  return FailureCount() ? 1 : 0;
}

inline float Rms(const std::vector<float>& x, size_t a, size_t b) {
  double s = 0.0;
  for (size_t i = a; i < b && i < x.size(); ++i) s += double(x[i]) * x[i];
  return b > a ? sqrtf(float(s / double(b - a))) : 0.0f;
}

inline float Db(float x) { return 20.0f * log10f(std::max(x, 1e-9f)); }

inline bool AllFinite(const std::vector<float>& x) {
  for (float v : x) if (!std::isfinite(v)) return false;
  return true;
}

inline float Peak(const std::vector<float>& x) {
  float p = 0.0f;
  for (float v : x) p = std::max(p, fabsf(v));
  return p;
}

}  // namespace vox_test

#endif  // VOX_TEST_TEST_UTIL_H_
