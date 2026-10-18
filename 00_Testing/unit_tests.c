#include "unit_tests.h"

int run_itinerary_unit_tests(void);
int run_optimizer_unit_tests(void);

#ifdef _WIN32
#include <io.h>
#define DUP _dup
#define DUP2 _dup2
#define CLOSE _close
#else
#include <unistd.h>
#define DUP dup
#define DUP2 dup2
#define CLOSE close
#endif

static int run_all_suites(void) {
    run_math_tests();
    run_lambert_tests();
    run_powered_flyby_tests();
    run_starting_orbit_tests();
    run_formats_tests();
    run_porkchop_tests();
    run_reference_defaults_tests();
    run_ksp_tests();
    int failed = finish_tests("KMAT unit checks");
    checks = failures = 0;
    failed |= run_itinerary_unit_tests();
    failed |= run_optimizer_unit_tests();
    return failed;
}

// Runs every unit suite and returns 1 if any check failed. Output of the suites is kept in a log file
// and only shown when something failed; the summary lines always are.
int run_unit_tests(void) {
    const char *log_path = "unit_tests.log";
    fflush(stdout);
    fflush(stderr);
    FILE *log = fopen(log_path, "w+");
    int saved_out = log != NULL ? DUP(1) : -1, saved_err = log != NULL ? DUP(2) : -1;
    if(saved_out < 0 || saved_err < 0) {
        if(log != NULL) fclose(log);
        return run_all_suites();
    }
    DUP2(fileno(log), 1);
    DUP2(fileno(log), 2);
    int failed = run_all_suites();
    fflush(stdout);
    fflush(stderr);
    DUP2(saved_out, 1);
    DUP2(saved_err, 2);
    CLOSE(saved_out);
    CLOSE(saved_err);

    rewind(log);
    char line[1024];
    while(fgets(line, sizeof(line), log) != NULL)
        if(failed || strstr(line, " unit checks: ") != NULL) fputs(line, stdout);
    fclose(log);
    if(!failed) remove(log_path);
    return failed;
}
