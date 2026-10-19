#ifndef AULA_CONSOLE_GATEWAY_TEST_H
#define AULA_CONSOLE_GATEWAY_TEST_H

/* Minimal assertion helpers shared by the console host unit tests. A failed
 * check is reported with its location and the test keeps running; main()
 * returns gateway_test_result() so `make test` stops on any failure. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gateway_test_failures = 0;

#define GATEWAY_CHECK(condition)                                           \
  do {                                                                     \
    if (!(condition)) {                                                    \
      (void)fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,         \
                    __LINE__, #condition);                                 \
      ++gateway_test_failures;                                             \
    }                                                                      \
  } while (0)

/* A request-log sink for tests that do not inspect the log. */
static inline void gateway_test_discard_log(void *context, const char *line) {
  (void)context;
  (void)line;
}

static int gateway_test_result(const char *name) {
  if (gateway_test_failures != 0) {
    (void)fprintf(stderr, "%s: %d check(s) failed\n", name,
                  gateway_test_failures);
    return EXIT_FAILURE;
  }
  (void)printf("%s: ok\n", name);
  return EXIT_SUCCESS;
}

#endif
