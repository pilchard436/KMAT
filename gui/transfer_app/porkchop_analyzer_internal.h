#ifndef KMAT_PORKCHOP_ANALYZER_INTERNAL_H
#define KMAT_PORKCHOP_ANALYZER_INTERNAL_H

/* State and helpers shared by the porkchop analyzer modules (core, groups, filter, io). */

#include "porkchop_analyzer.h"
#include "itinerary_calculator.h"
#include "porkchop_analyzer_tools.h"
#include "porkchop_analyzer_model.h"
#include "gui/drawing.h"
#include "gui/gui_manager.h"
#include "gui/settings.h"
#include "gui/info_win_manager.h"
#include "tools/file_io.h"

#include <gtk/gtk.h>

enum PaMinMaxType {PA_DEP, PA_ARR, PA_DUR, PA_TOTDV, PA_DEPDV, PA_SATDV};
enum PaYAxisType {PA_YAXIS_DUR, PA_YAXIS_ARRDATE};

extern int pa_num_deps, pa_num_itins;
extern struct ItinStep **pa_departures;
extern struct ItinStep **pa_dual_flyby_departures, **pa_dual_capture_departures;
extern int pa_dual_flyby_num_deps, pa_dual_capture_num_deps, pa_has_both_modes;
extern enum LastTransferType pa_last_transfer_type;
extern int pa_last_transfer_type_locked;
extern GObject *da_pa_porkchop, *da_pa_preview;
extern struct ItinStep *curr_transfer_pa;
extern struct ItinStep *pa_selected_arrival;
extern double current_date_pa;
extern int pa_num_groups;
extern struct PorkchopGroup *pa_groups;
extern struct PorkchopAnalyzerPoint *pa_porkchop_points;
extern ItinStepBinHeaderData pa_analysis_params;
extern CelestSystem *pa_system;
extern double pa_min_vals[6], pa_max_vals[6];
extern enum PaYAxisType pa_yaxis_type;
extern double pa_dep_periapsis, pa_arr_periapsis;
extern Screen *pa_porkchop_screen;
extern Camera *pa_itin_preview_camera;
extern gboolean *body_show_status_pa;
extern GObject *tf_pa_min_feedback[6];
extern GObject *tf_pa_max_feedback[6];
extern GObject *lb_pa_tfdate;
extern GObject *lb_pa_tfbody;
extern GObject *lb_pa_transfer_dv;
extern GObject *lb_pa_total_dv;
extern GObject *lb_pa_periapsis;
extern GObject *st_pa_step_group_selector;
extern GObject *vp_pa_groups;
extern GtkWidget *grid_pa_groups;

// porkchop_analyzer.c
void update_pa();
void update_pa_porkchop_diagram();
void update_pa_itinerary_preview();
void update_pa_selected_itinerary(void);
void reset_pa_porkchop_view();
void pa_update_body_show_status();
void pa_set_last_transfer_type(enum LastTransferType type, int locked);
double calc_step_dv_pa(struct ItinStep *step);
double calc_total_dv_pa(void);
double calc_periapsis_height_pa(void);

// porkchop_analyzer_groups.c
void free_all_porkchop_analyzer_itins();
void clear_porkchop_analyzer_analysis();
void update_group_overview();
void initialize_itinerary_groups();

// porkchop_analyzer_filter.c
void reset_min_max_feedback(int take_hidden_group_into_account);
void begin_pa_filter_entry_update(void);
void end_pa_filter_entry_update(void);
gboolean are_any_porkchop_points_in_filter(const double min[6], const double max[6]);
int apply_filter(void);
void connect_pa_filter_entry_validation(void);
void update_best_itin();
void analyze_departure_itins();

#endif
