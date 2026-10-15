#include "info_win_manager.h"
#include "orbit_calculator/transfer_calc.h"
#include "tools/thread_pool.h"
#include "tools/tool_funcs.h"


GObject *prog_window;
GObject *tf_prog_bar;
GObject *lb_prog_info;

GObject *msg_window;
GObject *lb_mw_msg;
static gint64 progress_started_at;
static gint64 last_console_progress_at;


void init_info_windows(GtkBuilder *builder) {
	prog_window = gtk_builder_get_object(builder, "progress_window");
	tf_prog_bar = gtk_builder_get_object(builder, "pb_pw_prog_bar");
	lb_prog_info = gtk_builder_get_object(builder, "lb_pw_prog_info");

	msg_window = gtk_builder_get_object(builder, "msg_window");
	lb_mw_msg = gtk_builder_get_object(builder, "lb_mw_msg");
}



// Function to update the progress window
static gboolean update_sc_ic_progress_window(gpointer data) {
	if (!gtk_widget_is_visible(GTK_WIDGET(prog_window))) {
		return G_SOURCE_REMOVE;  // Stop the timeout
	}
	struct Transfer_Calc_Status calc_status = get_current_transfer_calc_status();
	gint64 now = g_get_monotonic_time();
	double elapsed_seconds = (double)(now - progress_started_at) / G_USEC_PER_SEC;
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(tf_prog_bar), calc_status.progress);
	char progress_text[32];
	g_snprintf(progress_text, sizeof(progress_text), "%.0f%%", calc_status.progress * 100);
	gtk_progress_bar_set_text(GTK_PROGRESS_BAR(tf_prog_bar), progress_text);
	char s[256];
	if(get_thread_counter(3) != 0)
		sprintf(s, "Ending calculations: %.1f s\nDeparture dates: %d / %d complete\nItineraries found: %d",
			elapsed_seconds,
			calc_status.num_deps, (int) calc_status.jd_diff, calc_status.num_itins);
	else if(calc_status.post_processing)
		sprintf(s, "Scoring and refining routes: %.1f s\nDeparture dates: %d / %d complete\nItineraries found: %d",
			elapsed_seconds,
			calc_status.num_deps, (int) calc_status.jd_diff, calc_status.num_itins);
	else
		sprintf(s, "Search active: %.1f s\nDeparture dates: %d / %d complete\nItineraries found: %d",
			elapsed_seconds,
			calc_status.num_deps, (int) calc_status.jd_diff, calc_status.num_itins);
	gtk_label_set_text(GTK_LABEL(lb_prog_info), s);
	if(now - last_console_progress_at >= G_USEC_PER_SEC) {
		char console_status[160];
		g_snprintf(console_status, sizeof(console_status),
			"Search %.0f s: %d/%d dates complete, %d active",
			elapsed_seconds, calc_status.num_deps, (int)calc_status.jd_diff,
			calc_status.num_deps_active);
		show_progress(console_status, calc_status.progress, 1);
		last_console_progress_at = now;
	}

	return G_SOURCE_CONTINUE;  // Continue updating
}

void init_sc_ic_progress_window() {
	gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(tf_prog_bar), TRUE);
	progress_started_at = g_get_monotonic_time();
	last_console_progress_at = progress_started_at;
	gtk_widget_set_visible(GTK_WIDGET(prog_window), 1);
	// update progress window every 0.1s
	g_timeout_add(100, update_sc_ic_progress_window, NULL);
}

void end_sc_ic_progress_window() {
	gtk_widget_set_visible(GTK_WIDGET(prog_window), 0);
}

G_MODULE_EXPORT void end_progress_calculation() {
	get_incr_thread_counter(3);
}

void show_msg_window(char *msg) {
	gtk_label_set_text(GTK_LABEL(lb_mw_msg), msg);
	gtk_widget_set_visible(GTK_WIDGET(msg_window), 1);
}

G_MODULE_EXPORT gboolean on_hide_msg_window() {
	gtk_widget_set_visible(GTK_WIDGET(msg_window), 0);
	return TRUE;
}

G_MODULE_EXPORT void on_msg_window_ok() {
	gtk_widget_set_visible(GTK_WIDGET(msg_window), 0);
}

