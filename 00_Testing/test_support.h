#ifndef KMAT_TEST_SUPPORT_H
#define KMAT_TEST_SUPPORT_H

/* Minimal check helpers shared by the unit test executables; each executable counts its own checks. */

#include <math.h>
#include <stdio.h>

static int checks, failures;

static inline void check_true(const char *name, int condition) {
	checks++;
	if(condition) return;
	failures++;
	printf("FAIL: %s\n", name);
}

static inline void check_near(const char *name, double actual, double expected, double tolerance) {
	checks++;
	if(fabs(actual - expected) <= tolerance) return;
	failures++;
	printf("FAIL: %s (expected %.9g, got %.9g)\n", name, expected, actual);
}

/* Prints the summary line and returns the process exit code. */
static inline int finish_tests(const char *suite) {
	if(failures > 0) {
		printf("%s: %d total, %d passed, %d failed.\n", suite, checks, checks - failures, failures);
		return 1;
	}
	printf("%s: %d total, %d passed, 0 failed.\n", suite, checks, checks);
	return 0;
}

#endif
