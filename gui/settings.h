#ifndef KSP_SETTINGS_H
#define KSP_SETTINGS_H

#include <gtk/gtk.h>

enum DefaultDepartureDate {
	DEFAULT_DEPARTURE_DATE_1950,
	DEFAULT_DEPARTURE_DATE_TODAY
};


void init_global_settings(GtkBuilder *builder);
/* Tries to connect to KSP with the saved host and port in the background. */
void ksp_auto_connect(void);
enum DateType get_settings_datetime_type();
char *get_settings_default_system();
enum DefaultDepartureDate get_settings_default_departure_date();


// Handler --------------------------
G_MODULE_EXPORT void on_change_datetime_type();
G_MODULE_EXPORT void on_change_default_system();
G_MODULE_EXPORT void on_change_default_departure_date();
G_MODULE_EXPORT void on_settings_ksp_connect(GtkWidget *widget, gpointer data);

#endif //KSP_SETTINGS_H
