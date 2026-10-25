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

void set_calculation_progress_phase(Transfer_Progress_Phase phase, int transfer_type,
		int phase_done, int phase_total) {
	thread_progress_and_prop_info.phase = phase;
	thread_progress_and_prop_info.phase_transfer_type = transfer_type;
	thread_progress_and_prop_info.phase_done = phase_done;
	thread_progress_and_prop_info.phase_total = phase_total;
}

void increment_calculation_progress(void) {
	if(thread_progress_and_prop_info.phase_done < thread_progress_and_prop_info.phase_total)
		thread_progress_and_prop_info.phase_done++;
}

struct Transfer_Calc_Status get_current_transfer_calc_status() {
	int total_departures = thread_progress_and_prop_info.num_departures;
	int num_deps = get_thread_counter(1);
	int num_deps_started = get_thread_counter(0);
	int num_deps_active = get_thread_counter(4);
	if(total_departures > 0 && num_deps_started > total_departures) num_deps_started = total_departures;
	Transfer_Progress_Phase phase = thread_progress_and_prop_info.phase;
	int phase_done = phase == TRANSFER_PROGRESS_SEARCH ? num_deps :
		thread_progress_and_prop_info.phase_done;
	int phase_total = phase == TRANSFER_PROGRESS_SEARCH ? total_departures :
		thread_progress_and_prop_info.phase_total;
	double progress = phase_total > 0 ?
		fmin(1, (double)phase_done / phase_total) :
		phase == TRANSFER_PROGRESS_FINALIZING ? 0 : 1;

	return (struct Transfer_Calc_Status) {
		.num_itins = get_thread_counter(2),
		.num_deps = num_deps,
		.num_deps_started = num_deps_started,
		.num_deps_active = num_deps_active,
		.total_departures = total_departures,
		.phase = phase,
		.phase_done = phase_done,
		.phase_total = phase_total,
		.phase_transfer_type = thread_progress_and_prop_info.phase_transfer_type,
		.progress = progress
	};
}
