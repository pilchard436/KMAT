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

static const char *transfer_progress_mode_name(int transfer_type) {
	return transfer_type == TF_CAPTURE ? "Capture" :
		transfer_type == TF_FLYBY ? "Fly-By" : "combined";
}

static const char *transfer_progress_phase_verb(Transfer_Progress_Phase phase) {
	switch(phase) {
		case TRANSFER_PROGRESS_SEARCH: return "Searching";
		case TRANSFER_PROGRESS_PREPARING:
		case TRANSFER_PROGRESS_PREPARING_CAPTURE: return "Preparing";
		case TRANSFER_PROGRESS_SCORING: return "Scoring";
		case TRANSFER_PROGRESS_REPAIRING: return "Repairing";
		case TRANSFER_PROGRESS_REFINING: return "Refining";
		case TRANSFER_PROGRESS_OPTIMIZING_BOTH: return "Optimizing";
		case TRANSFER_PROGRESS_FILTERING: return "Filtering";
		case TRANSFER_PROGRESS_FINALIZING: return "Finalizing";
	}
	return "Working";
}


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
	if(calc_status.phase == TRANSFER_PROGRESS_FINALIZING)
		gtk_progress_bar_pulse(GTK_PROGRESS_BAR(tf_prog_bar));
	else
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(tf_prog_bar), calc_status.progress);
	char progress_text[32];
	if(calc_status.phase == TRANSFER_PROGRESS_FINALIZING)
		g_snprintf(progress_text, sizeof(progress_text), "Finishing");
	else
		g_snprintf(progress_text, sizeof(progress_text), "%d%%", (int)(calc_status.progress * 100));
	gtk_progress_bar_set_text(GTK_PROGRESS_BAR(tf_prog_bar), progress_text);
	char s[256];
	if(get_thread_counter(3) != 0) {
		g_snprintf(s, sizeof(s),
			"Cancellation requested; finishing active work: %.1f s\n%d workers active; %d candidate routes",
			elapsed_seconds, calc_status.num_deps_active, calc_status.num_itins);
	} else if(calc_status.phase == TRANSFER_PROGRESS_SEARCH) {
		g_snprintf(s, sizeof(s),
			"Searching transfers: %d / %d departure dates complete\nElapsed: %.1f s; %d workers active; %d candidate routes",
			calc_status.phase_done, calc_status.phase_total, elapsed_seconds,
			calc_status.num_deps_active, calc_status.num_itins);
	} else if(calc_status.phase == TRANSFER_PROGRESS_FINALIZING) {
		g_snprintf(s, sizeof(s),
			"Finalizing results; route checks and data preparation are still running\nElapsed: %.1f s; %d workers active; %d candidate routes",
			elapsed_seconds, calc_status.num_deps_active, calc_status.num_itins);
	} else if(calc_status.phase == TRANSFER_PROGRESS_OPTIMIZING_BOTH) {
		g_snprintf(s, sizeof(s),
			"Optimizing for Fly-By and Capture\nElapsed: %.1f s; departure dates: %d / %d; %d candidate routes",
			elapsed_seconds, calc_status.num_deps, calc_status.total_departures,
			calc_status.num_itins);
	} else {
		g_snprintf(s, sizeof(s), "%s %s routes: %d / %d complete\nElapsed: %.1f s; departure dates: %d / %d; %d candidate routes",
			transfer_progress_phase_verb(calc_status.phase),
			transfer_progress_mode_name(calc_status.phase_transfer_type),
			calc_status.phase_done, calc_status.phase_total, elapsed_seconds,
			calc_status.num_deps, calc_status.total_departures, calc_status.num_itins);
	}
	gtk_label_set_text(GTK_LABEL(lb_prog_info), s);
	if(now - last_console_progress_at >= G_USEC_PER_SEC) {
		char console_status[160];
		if(calc_status.phase == TRANSFER_PROGRESS_FINALIZING)
			g_snprintf(console_status, sizeof(console_status), "Finalizing results");
		else if(calc_status.phase == TRANSFER_PROGRESS_OPTIMIZING_BOTH)
			g_snprintf(console_status, sizeof(console_status), "Optimizing for Fly-By and Capture");
		else
			g_snprintf(console_status, sizeof(console_status), "%s: %d/%d complete",
				transfer_progress_phase_verb(calc_status.phase),
				calc_status.phase_done, calc_status.phase_total);
		show_progress(console_status, calc_status.progress, 1);
		last_console_progress_at = now;
	}

	return G_SOURCE_CONTINUE;  // Continue updating
}

void init_sc_ic_progress_window() {
	gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(tf_prog_bar), TRUE);
	gtk_progress_bar_set_pulse_step(GTK_PROGRESS_BAR(tf_prog_bar), 0.05);
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

static const void *msg_window_owner;

void show_msg_window(char *msg) {
	msg_window_owner = NULL;
	gtk_label_set_text(GTK_LABEL(lb_mw_msg), msg);
	gtk_widget_set_visible(GTK_WIDGET(msg_window), 1);
}

void show_msg_window_owned(char *msg, const void *owner) {
	show_msg_window(msg);
	msg_window_owner = owner;
}

int msg_window_shows_owner(const void *owner) {
	return owner != NULL && owner == msg_window_owner && gtk_widget_get_visible(GTK_WIDGET(msg_window));
}

void update_msg_window_if_owned(char *msg, const void *owner) {
	if(msg_window_shows_owner(owner)) gtk_label_set_text(GTK_LABEL(lb_mw_msg), msg);
}

G_MODULE_EXPORT gboolean on_hide_msg_window() {
	gtk_widget_set_visible(GTK_WIDGET(msg_window), 0);
	return TRUE;
}

G_MODULE_EXPORT void on_msg_window_ok() {
	gtk_widget_set_visible(GTK_WIDGET(msg_window), 0);
}
