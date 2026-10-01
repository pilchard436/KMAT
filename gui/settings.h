#ifndef KSP_SETTINGS_H
#define KSP_SETTINGS_H

#include <gtk/gtk.h>

enum DefaultDepartureDate {
	DEFAULT_DEPARTURE_DATE_1950,
	DEFAULT_DEPARTURE_DATE_TODAY
};

void init_global_settings(GtkBuilder *builder);
enum DateType get_settings_datetime_type();
char *get_settings_default_system();
enum DefaultDepartureDate get_settings_default_departure_date();


// Handler --------------------------
G_MODULE_EXPORT void on_change_datetime_type();
G_MODULE_EXPORT void on_change_default_system();
G_MODULE_EXPORT void on_change_default_departure_date();

#endif //KSP_SETTINGS_H
