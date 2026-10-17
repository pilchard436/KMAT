#ifndef KSP_INFO_WIN_MANAGER_H
#define KSP_INFO_WIN_MANAGER_H

#include <gtk/gtk.h>

void init_info_windows(GtkBuilder *builder);
void init_sc_ic_progress_window();
void end_sc_ic_progress_window();
void show_msg_window(char *msg);
/* The owner tags who put the text there, so only that owner can refresh it while the window is open. */
void show_msg_window_owned(char *msg, const void *owner);
int msg_window_shows_owner(const void *owner);
void update_msg_window_if_owned(char *msg, const void *owner);


// Handler ----------------------------
G_MODULE_EXPORT void end_progress_calculation();
G_MODULE_EXPORT gboolean on_hide_msg_window();
G_MODULE_EXPORT void on_msg_window_ok();

#endif //KSP_INFO_WIN_MANAGER_H
