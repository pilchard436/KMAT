#include "transfer_search_internal.h"
#include "tools/tool_funcs.h"
#include "tools/thread_pool.h"
#include <math.h>
#include <stdio.h>
#include <sys/time.h>

struct Thread_Progress_and_Prop_Info thread_progress_and_prop_info;

double progress_clock(void) {
	struct timeval now;
	gettimeofday(&now, NULL);
	return now.tv_sec + now.tv_usec / 1e6;
}

void show_departure_progress(double total_departures) {
	int completed = get_thread_counter(1);
	int started = get_thread_counter(0);
	int active = get_thread_counter(4);
	if(total_departures > 0 && started > total_departures) started = (int)total_departures;
	char status[128];
	snprintf(status, sizeof(status), "Departure dates: %d/%d complete, %d started, %d active",
		completed, (int)total_departures, started, active);
	Transfer_Calc_Status calc_status = get_current_transfer_calc_status();
	show_progress(status, calc_status.progress, 1);
}

struct Transfer_Calc_Status get_current_transfer_calc_status() {
	double jd_diff = thread_progress_and_prop_info.jd_max_dep-thread_progress_and_prop_info.jd_min_dep+1;
	int num_deps = get_thread_counter(1);
	int num_deps_started = get_thread_counter(0);
	int num_deps_active = get_thread_counter(4);
	int initial_samples = get_thread_counter(INITIAL_SAMPLE_PROGRESS_COUNTER);
	if(jd_diff > 0 && num_deps_started > jd_diff) num_deps_started = (int)jd_diff;
	double completed_date_progress = jd_diff > 0 ? fmin(1, (double)num_deps / jd_diff) : 0;
	double initial_sample_progress = thread_progress_and_prop_info.total_initial_samples > 0 ?
		fmin(1, (double)initial_samples / thread_progress_and_prop_info.total_initial_samples) :
		completed_date_progress;
	double progress = 0.75 * initial_sample_progress + 0.25 * completed_date_progress;
	if(completed_date_progress >= 1) progress = 1;
	int post_processing = thread_progress_and_prop_info.has_post_processing && completed_date_progress >= 1;
	if(thread_progress_and_prop_info.has_post_processing) {
		// Share of the bar for scoring/refinement, predicted from the route count and elapsed search time.
		double now = progress_clock();
		if(post_processing && thread_progress_and_prop_info.search_end_time == 0)
			thread_progress_and_prop_info.search_end_time = now;
		double search_fraction = post_processing ? 1 : fmax(progress, 0.05);
		double search_seconds = (post_processing ? thread_progress_and_prop_info.search_end_time : now) -
			thread_progress_and_prop_info.search_start_time;
		double predicted_post_seconds = ((double)get_thread_counter(2) / search_fraction +
			POWERED_PROGRESS_REFINE_UNITS) * POWERED_PROGRESS_SECONDS_PER_UNIT;
		double predicted_search_seconds = search_seconds / search_fraction;
		double post_share = predicted_post_seconds / (predicted_search_seconds + predicted_post_seconds);
		if(post_processing) {
			int post_total = get_thread_counter(POWERED_PROGRESS_TOTAL_COUNTER);
			double post_progress = post_total > 0 ?
				fmin(1, (double)get_thread_counter(POWERED_PROGRESS_DONE_COUNTER) / post_total) : 0;
			progress = (1 - post_share) + post_share * post_progress;
		} else {
			progress *= 1 - post_share;
		}
		// Estimates shift as the search proceeds; the bar must never move backwards.
		progress = fmax(progress, thread_progress_and_prop_info.max_progress);
		thread_progress_and_prop_info.max_progress = progress;
	}

	return (struct Transfer_Calc_Status) {
		.num_itins = get_thread_counter(2),
		.num_deps = num_deps,
		.num_deps_started = num_deps_started,
		.num_deps_active = num_deps_active,
		.jd_diff = jd_diff,
		.progress = progress,
		.post_processing = post_processing
	};
}
