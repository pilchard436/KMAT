#include <stdio.h>
#include <stdatomic.h>
#include "tool_funcs.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static atomic_flag progress_output_lock = ATOMIC_FLAG_INIT;
static int progress_cursor_hidden;
#ifdef _WIN32
static HANDLE progress_console_handle;
static CONSOLE_CURSOR_INFO original_cursor_info;
#endif

static void lock_progress_output(void) {
    while(atomic_flag_test_and_set_explicit(&progress_output_lock, memory_order_acquire)) {}
}

static void unlock_progress_output(void) {
    atomic_flag_clear_explicit(&progress_output_lock, memory_order_release);
}

static void hide_progress_cursor(void) {
    if(progress_cursor_hidden) return;
#ifdef _WIN32
    HANDLE console_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO cursor_info;
    if(console_handle == INVALID_HANDLE_VALUE || !GetConsoleCursorInfo(console_handle, &cursor_info)) return;
    original_cursor_info = cursor_info;
    cursor_info.bVisible = FALSE;
    if(SetConsoleCursorInfo(console_handle, &cursor_info)) {
        progress_console_handle = console_handle;
        progress_cursor_hidden = 1;
    }
#else
    if(!isatty(fileno(stdout))) return;
    fputs("\033[?25l", stdout);
    fflush(stdout);
    progress_cursor_hidden = 1;
#endif
}

static void restore_progress_cursor(void) {
    if(!progress_cursor_hidden) return;
#ifdef _WIN32
    SetConsoleCursorInfo(progress_console_handle, &original_cursor_info);
#else
    fputs("\033[?25h", stdout);
    fflush(stdout);
#endif
    progress_cursor_hidden = 0;
}

int user_selection(char *title, char *options, char *question) {
    printf("\n\n%s\n", title);
    print_separator(49);
    int i = 1;  // 1, because see while loop
    int j = 0;

    while(options[i-1] != 0) {  // -1, because i is the first character of the option (character in front is relevant)
        if(options[0] == ' ') options = &options[i+1];      // skip first character if space

        while(options[i] != 0 && options[i] != ';') i++;

        printf("| - % 2d: %.*s", j, i, options);
        for(int k = 0; k < (47-(i+8))/8; k++) printf("\t");     // for a max line length of 32 (next line for initial \t)
        printf("\t|\n");

        options = &options[i+1];
        i = 0;
        j++;
    }

    print_separator(49);
    printf("\n%s", question);

    int sel;
    scanf(" %d", &sel);
    if(sel < 0 || sel >= j) sel = 0;    // if sel is not one of the options, set to 0
    printf("\n");
    print_separator(49);
    print_separator(49);
    printf("\n");
    return sel;
}


void print_separator(int x) {
    for(int i = 0; i < x; i++) printf("_");
    printf("\n\n");
}


int strcicmp(char const *a, char const *b)
{
    for (;; a++, b++) {
        int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (d != 0 || !*a)
            return d;
    }
}


void show_progress(char *text, double progress, double total) {
    double percentage = (progress / total) * 100.0;
    char line[256];
    snprintf(line, sizeof(line), "%s: %.2f%%", text, percentage);
    lock_progress_output();
    if(progress < total) hide_progress_cursor();
    printf("\r%-80s", line);
    fflush(stdout);
    if(total > 0 && progress >= total) restore_progress_cursor();
    unlock_progress_output();
}

void finish_progress(void) {
    lock_progress_output();
    restore_progress_cursor();
    unlock_progress_output();
}
