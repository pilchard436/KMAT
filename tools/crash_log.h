#ifndef KMAT_CRASH_LOG_H
#define KMAT_CRASH_LOG_H

// Appends the exception code and a module-relative stack to kmat_crash.txt when the app crashes or aborts (Windows only).
void install_crash_log(void);

#endif
