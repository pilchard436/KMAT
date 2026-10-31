#ifndef KSP_ITINERARY_CALCULATOR_H
#define KSP_ITINERARY_CALCULATOR_H

#include <gtk/gtk.h>
#include "orbitlib.h"

void init_itinerary_calculator(GtkBuilder *builder);
int ic_select_ksp_start(CelestSystem *system, Body *departure_body, double departure_jd);
void ic_clear_ksp_start(void);
void ic_set_ksp_note(const char *text);
void ic_change_date_type(enum DateType old_date_type, enum DateType new_date_type);
void ic_initialize_date_defaults(void);
void connect_itinerary_date_entry_validation(GtkEntry *entry);
void set_ic_default_system(char *system_name);
void reset_ic();
/* Styles an event box as a hoverable info icon. */
void setup_ic_info_icon_hover(GtkWidget *widget);



// Handler ----------------------

G_MODULE_EXPORT void on_calc_ic();
G_MODULE_EXPORT void on_ic_system_change();
G_MODULE_EXPORT void on_ic_central_body_change();
G_MODULE_EXPORT void on_get_ic_ref_values();
G_MODULE_EXPORT void on_ic_date_icon_release(GtkEntry *entry, GtkEntryIconPosition icon_pos, gpointer data);
G_MODULE_EXPORT void on_ic_time_limit_toggled(GtkToggleButton *button, gpointer data);

#endif //KSP_ITINERARY_CALCULATOR_H
