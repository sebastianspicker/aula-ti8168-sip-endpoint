#ifndef AULA_SIPD_TEST_H
#define AULA_SIPD_TEST_H

/* Minimal assertion helpers shared by the host unit tests. A failed check is
 * reported with its location and the test keeps running; main() returns
 * aula_test_result() so CTest sees a non-zero exit for any failure. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int aula_test_failures = 0;

#define AULA_CHECK(condition)                                              \
  do {                                                                     \
    if (!(condition)) {                                                    \
      (void)fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,         \
                    __LINE__, #condition);                                 \
      ++aula_test_failures;                                                \
    }                                                                      \
  } while (0)

#define AULA_CHECK_STATUS(expression, expected)                            \
  do {                                                                     \
    int aula_check_actual_ = (int)(expression);                            \
    if (aula_check_actual_ != (int)(expected)) {                           \
      (void)fprintf(stderr, "%s:%d: %s returned %d, expected %d\n",        \
                    __FILE__, __LINE__, #expression, aula_check_actual_,   \
                    (int)(expected));                                      \
      ++aula_test_failures;                                                \
    }                                                                      \
  } while (0)

static int aula_test_result(const char *name) {
  if (aula_test_failures != 0) {
    (void)fprintf(stderr, "%s: %d check(s) failed\n", name,
                  aula_test_failures);
    return EXIT_FAILURE;
  }
  (void)printf("%s: ok\n", name);
  return EXIT_SUCCESS;
}

#endif
