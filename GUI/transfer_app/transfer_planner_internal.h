#ifndef KMAT_TRANSFER_PLANNER_INTERNAL_H
#define KMAT_TRANSFER_PLANNER_INTERNAL_H

/* State and helpers shared by the transfer planner modules (core, view, panel, io). */

#include "transfer_planner.h"
#include "gui/drawing.h"
#include "gui/gui_manager.h"
#include "gui/settings.h"
#include "gui/info_win_manager.h"
#include "orbit_calculator/powered_flyby_optimizer.h"

#include <gtk/gtk.h>

extern struct ItinStep *curr_transfer_tp;
extern CelestSystem *tp_system;
extern Camera *tp_system_camera;

extern GObject *da_tp;
extern GObject *cb_tp_system;
extern int tp_loaded_system_entry;
extern GObject *cb_tp_tfbody;
extern GObject *lb_tp_date;
extern GObject *tb_tp_tfdate;
extern GObject *bt_tp_10y6hp;
extern GObject *bt_tp_10y6hm;
extern GObject *bt_tp_1y1hp;
extern GObject *bt_tp_1y1hm;
extern GObject *bt_tp_1m30d10minp;
extern GObject *bt_tp_1m30d10minm;
extern GObject *bt_tp_1d1minp;
extern GObject *bt_tp_1d1minm;
extern GObject *lb_tp_transfer_dv;
extern GObject *lb_tp_total_dv;
extern GObject *lb_tp_dv_warning;
extern GObject *lb_tp_param_labels;
extern GObject *lb_tp_param_values;
extern GObject *vp_tp_bodies;
extern GtkWidget *grid_tp_bodies;
extern GtkWidget *tb_tp_animate, *cb_tp_anim_speed;

extern gboolean *body_show_status_tp, fix_tfbody;
extern double current_date_tp;
extern enum LastTransferType tp_last_transfer_type;
extern int tp_last_transfer_type_locked;
extern int tp_setting_last_transfer_type;
extern GtkWidget *tp_transfer_type_buttons[2];
extern ItinSearchOptions tp_search_options;
extern double tp_dep_periapsis, tp_arr_periapsis;

// The body the view is centered on (NULL: central body), and whether it follows the spacecraft instead.
extern Body *tp_focus_body;
extern int tp_focus_spacecraft;
// Set when the flyby iteration did not settle and the shown delta-v is only the last iterate.
extern int tp_route_unconverged;
// Identifies the itinerary details text in the shared message window.
extern char tp_details_owner;

// transfer_planner.c
double calc_step_dv(struct ItinStep *step);
double calc_total_dv();
double calc_periapsis_height_tp();
int tp_resolve_route(void);
void update_itinerary();
void tp_set_last_transfer_type(enum LastTransferType type, int locked);
void sort_transfer_dates(struct ItinStep *tf);
void remove_all_transfers();

// transfer_planner_view.c
void update_tp_system_view();
void tp_update_show_body_list();
void show_bodies_of_itinerary(struct ItinStep *step);
void tp_update_bodies();
void tp_stop_animation(void);
void on_tp_screen_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer *ptr);
void on_tp_screen_resize(GtkWidget *widget, cairo_t *cr, gpointer *ptr);
void on_tp_screen_mouse_move(GtkWidget *widget, GdkEventButton *event, gpointer *ptr);
gboolean on_tp_screen_button_press(GtkWidget *widget, GdkEventButton *event, gpointer ptr);
void on_tp_view_destroyed(GtkWidget *widget, gpointer data);

// transfer_planner_panel.c
int find_closest_transfer(struct ItinStep *step);

#endif
