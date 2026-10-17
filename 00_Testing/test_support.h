#ifndef KMAT_TEST_SUPPORT_H
#define KMAT_TEST_SUPPORT_H

/* Minimal check helpers shared by the unit test executables; each executable counts its own checks. */

#include "geometrylib_vec.h"
#include "orbitlib_datetime.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Defined in test_support.c so that every suite of an executable shares them. */
extern int checks, failures;

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

static inline int expect_int(const char *name, int actual, int expected) {
	checks++;
	if(actual == expected) return 0;
	failures++;
	printf("FAIL: %s (expected %d, got %d)\n", name, expected, actual);
	return 1;
}

static inline int expect_near(const char *name, double actual, double expected, double tolerance) {
	checks++;
	if(fabs(actual - expected) <= tolerance) return 0;
	failures++;
	printf("FAIL: %s (expected %.12f, got %.12f)\n", name, expected, actual);
	return 1;
}

static inline int expect_vector3(const char *name, Vector3 actual, Vector3 expected, double tolerance) {
	return expect_near(name, actual.x, expected.x, tolerance) + expect_near(name, actual.y, expected.y, tolerance) +
		expect_near(name, actual.z, expected.z, tolerance);
}

static inline int expect_contains(const char *name, const char *actual, const char *expected) {
	checks++;
	if(strstr(actual, expected) != NULL) return 0;
	failures++;
	printf("FAIL: %s (missing '%s')\n", name, expected);
	return 1;
}

static inline int expect_datetime(const char *name, Datetime actual, Datetime expected, double tolerance) {
	checks++;
	int mismatches = actual.y != expected.y || actual.m != expected.m || actual.d != expected.d || actual.h != expected.h ||
		actual.min != expected.min || actual.date_type != expected.date_type || fabs(actual.s - expected.s) > tolerance;
	if(!mismatches) return 0;
	failures++;
	printf("FAIL: %s\n", name);
	return 1;
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
