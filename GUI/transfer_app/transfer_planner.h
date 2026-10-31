#ifndef KSP_TRANSFER_PLANNER_H
#define KSP_TRANSFER_PLANNER_H


#include <gtk/gtk.h>
#include "orbitlib.h"
#include "tools/file_io.h"

void init_transfer_planner(GtkBuilder *builder);
void tp_select_system(int index);
void tp_change_date_type(enum DateType old_date_type, enum DateType new_date_type);
void update();
void update_date_label();
void update_transfer_panel();

void end_transfer_planner();
// Shows a copy of the route as if it had been saved and loaded, and switches to the Transfer Planner.
void tp_open_itinerary(struct ItinStep *itin, CelestSystem *system, ItinSearchOptions search_options,
	double dep_periapsis, double arr_periapsis);




// Handler --------------------------------------------------
G_MODULE_EXPORT void on_tp_system_change();
G_MODULE_EXPORT void on_transfer_planner_draw(GtkWidget *widget, cairo_t *cr, gpointer data);
G_MODULE_EXPORT void on_body_toggle(GtkWidget* widget, gpointer data);
G_MODULE_EXPORT void on_change_date(GtkWidget* widget, gpointer data);
G_MODULE_EXPORT void on_tp_reset_clocktime(GtkWidget* widget, gpointer data);
G_MODULE_EXPORT void on_tp_switch_clocktime_date(GtkWidget* widget, gpointer data);
G_MODULE_EXPORT void on_prev_transfer(GtkWidget* widget, gpointer data);
G_MODULE_EXPORT void on_next_transfer(GtkWidget* widget, gpointer data);
G_MODULE_EXPORT void on_transfer_body_change(GtkWidget* widget, gpointer data);
G_MODULE_EXPORT void on_last_transfer_type_changed_tp(GtkWidget* widget, gpointer data);
G_MODULE_EXPORT void on_toggle_transfer_date_lock(GtkWidget* widget, gpointer data);
G_MODULE_EXPORT void on_goto_transfer_date(GtkWidget* widget, gpointer data);
G_MODULE_EXPORT void on_transfer_body_select(GtkWidget* widget, gpointer data);
G_MODULE_EXPORT void on_find_closest_transfer(GtkWidget* widget, gpointer data);
//G_MODULE_EXPORT void on_find_itinerary(GtkWidget* widget, gpointer data);
G_MODULE_EXPORT void on_show_itin_overview();
G_MODULE_EXPORT void on_save_itinerary(GtkWidget* widget, gpointer data);
G_MODULE_EXPORT void on_load_itinerary(GtkWidget* widget, gpointer data);



#endif //KSP_TRANSFER_PLANNER_H
