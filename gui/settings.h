#ifndef KSP_SETTINGS_H
#define KSP_SETTINGS_H

#include <gtk/gtk.h>

void init_global_settings(GtkBuilder *builder);
/* Tries to connect to KSP with the saved host and port in the background. */
void ksp_auto_connect(void);
enum DateType get_settings_datetime_type();
void set_settings_datetime_type(enum DateType date_type);
char *get_settings_default_system();


// Handler --------------------------
G_MODULE_EXPORT void on_change_default_system();
G_MODULE_EXPORT void on_change_seed_budget();
G_MODULE_EXPORT void on_settings_ksp_connect(GtkWidget *widget, gpointer data);

#endif //KSP_SETTINGS_H
