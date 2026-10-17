#ifndef KSP_TESTING_H
#define KSP_TESTING_H

int run_tests();

// Runs the unit suites compiled into the application (KMAT --unit-tests); returns 1 on any failure.
int run_unit_tests(void);

#endif //KSP_TESTING_H
