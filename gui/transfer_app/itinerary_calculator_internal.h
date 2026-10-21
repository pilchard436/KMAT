#ifndef KMAT_ITINERARY_CALCULATOR_INTERNAL_H
#define KMAT_ITINERARY_CALCULATOR_INTERNAL_H

/* State and helpers shared by the itinerary calculator modules (core, dates, run). */

#include "itinerary_calculator.h"
#include "orbit_calculator/transfer_calc.h"
#include "gui/gui_manager.h"
#include "gui/settings.h"
#include "gui/info_win_manager.h"
#include "starting_orbit_editor.h"

#include <gtk/gtk.h>

extern GObject *tf_ic_window;
extern GObject *cb_ic_system;
extern GObject *cb_ic_central_body;
extern GObject *lb_ic_central_body;
extern GObject *cb_ic_depbody;
extern GObject *cb_ic_arrbody;
extern GObject *tf_ic_mindepdate;
extern GObject *tf_ic_maxdepdate;
extern GObject *tf_ic_maxarrdate;
extern GObject *tf_ic_maxdur;
extern GObject *rb_ic_maxarrdate;
extern GObject *rb_ic_maxdur;
extern GObject *cb_ic_transfertype;
extern GtkWidget *cb_ic_allow_dsm;
extern GtkWidget *cb_ic_allow_resonant;
extern GtkWidget *cb_ic_moon_aware_flybys;
extern GtkWidget *cb_ic_moon_assist_arrival;
extern GObject *tf_ic_depdv;
extern GObject *tf_ic_satdv;
extern GObject *vp_ic_fbbodies;
extern GtkWidget *grid_ic_fbbodies;
extern GtkWidget *ic_latest_dep_minus_month_button;
extern GtkWidget *ic_latest_dep_plus_month_button;
extern GtkWidget *ic_latest_arr_minus_month_button;
extern GtkWidget *ic_latest_arr_plus_month_button;
extern gboolean ic_changing_date_type;
extern gboolean ic_syncing_time_limits;
extern gboolean ic_setting_ksp_start_date;
extern gboolean ic_ksp_start_active;
extern double ic_ksp_start_jd;
extern Body *ic_ksp_start_body;

extern CelestSystem *ic_system;
extern double ic_dep_periapsis, ic_arr_periapsis;

extern struct Itin_Calc_Data ic_calc_data;
extern struct Itin_Calc_Results ic_results;

GtkWidget * ic_update_seq_body_grid(GObject *viewport, GtkWidget *grid);
int get_num_selected_bodies_from_grid(GtkWidget *grid);
Body ** get_bodies_from_grid(GtkWidget *grid, int num_bodies);

// itinerary_calculator_dates.c
void connect_date_adjust_buttons(GtkBuilder *builder, const char *prefix, GtkEntry *target);
void update_month_adjust_button_labels(enum DateType date_type);
void on_ic_earliest_dep_changed(GtkEditable *editable, gpointer user_data);
void on_ic_latest_arrival_changed(GtkEditable *editable, gpointer user_data);
void on_ic_max_duration_changed(GtkEditable *editable, gpointer user_data);
gboolean update_ic_reference_defaults(void);
void on_ic_reference_input_changed(GtkWidget *widget, gpointer user_data);

#endif
