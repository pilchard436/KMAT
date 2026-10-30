#include "integration_common.h"
#include "orbit_calculator/flyby_policy.h"

#include <sys/time.h>

#define REPLAY_RATIO_BINS 12
#define REPLAY_MAX_DEPTH 16

// Ratio binned in steps of 0.5 up to the last, open-ended bin.
static int replay_ratio_bin(double ratio) {
	int bin = (int)(ratio / 0.5);
	return bin < 0 ? 0 : bin >= REPLAY_RATIO_BINS ? REPLAY_RATIO_BINS - 1 : bin;
}

static double replay_period_days(const Body *body, const CelestSystem *system) {
	return 2 * M_PI * sqrt(pow(body->orbit.a, 3) / system->cb->mu) / 86400;
}

// Synodic period of two bodies in days; the longer period when both are equal.
static double replay_synodic_days(const Body *a, const Body *b, const CelestSystem *system) {
	double pa = replay_period_days(a, system), pb = replay_period_days(b, system);
	double diff = fabs(1 / pa - 1 / pb);
	return diff > 0 ? 1 / diff : fmax(pa, pb);
}

struct ReplayRatioStats {
	const char *name;
	long long legs[REPLAY_RATIO_BINS], routes_with_at_least[REPLAY_RATIO_BINS];
	double best_with_at_least[REPLAY_RATIO_BINS], best_without_at_least[REPLAY_RATIO_BINS];
};

static void replay_ratio_stats_init(struct ReplayRatioStats *stats, const char *name) {
	memset(stats, 0, sizeof(*stats));
	stats->name = name;
	for(int i = 0; i < REPLAY_RATIO_BINS; i++) stats->best_with_at_least[i] = stats->best_without_at_least[i] = DBL_MAX;
}

// A route whose longest intermediate-leg ratio falls in `max_bin` counts for every threshold up to it, and towards
// the best delta-v that survives a cap below every threshold above it.
static void replay_ratio_stats_add_route(struct ReplayRatioStats *stats, int max_bin, double total) {
	for(int bin = 0; bin < REPLAY_RATIO_BINS; bin++) {
		if(bin <= max_bin) {
			stats->routes_with_at_least[bin]++;
			if(total < stats->best_with_at_least[bin]) stats->best_with_at_least[bin] = total;
		} else if(total < stats->best_without_at_least[bin]) stats->best_without_at_least[bin] = total;
	}
}

static void replay_ratio_stats_print(const struct ReplayRatioStats *stats) {
	printf("Intermediate legs by duration / %s (final legs excluded):\n", stats->name);
	for(int bin = 0; bin < REPLAY_RATIO_BINS; bin++)
		printf("  >= %4.1f: %7lld legs; routes with such a leg: %7lld, best %.1f m/s; best without: %.1f m/s\n", bin * 0.5,
			stats->legs[bin], stats->routes_with_at_least[bin], stats->best_with_at_least[bin], stats->best_without_at_least[bin]);
}

// Leg durations relative to Hohmann, synodic and orbital periods, route depth, and the best delta-v with and without
// routes exceeding each threshold, over every itinerary in the results. Only intermediate legs are measured since a
// cap would not apply to the final leg.
static void report_replay_route_statistics(Itin_Calc_Results *results, const Itin_Calc_Data *calc_data) {
	CelestSystem *system = calc_data->seq_info.to_target.system;
	struct ReplayRatioStats hohmann_stats, synodic_stats, period_stats;
	replay_ratio_stats_init(&hohmann_stats, "Hohmann duration");
	replay_ratio_stats_init(&synodic_stats, "synodic period");
	replay_ratio_stats_init(&period_stats, "longer orbital period");
	long long depth_counts[REPLAY_MAX_DEPTH + 1] = {0}, resonant_legs = 0;
	double best_total = DBL_MAX, best_by_depth[REPLAY_MAX_DEPTH + 1], best_up_to_depth[REPLAY_MAX_DEPTH + 1];
	int long_leg_examples = 0;
	for(int i = 0; i <= REPLAY_MAX_DEPTH; i++) best_by_depth[i] = best_up_to_depth[i] = DBL_MAX;
	for(int d = 0; d < results->num_deps; d++) {
		int count = get_number_of_itineraries(results->departures[d]);
		struct ItinStep **leaves = malloc((size_t)(count > 0 ? count : 1) * sizeof(*leaves));
		if(leaves == NULL) continue;
		int stored = 0;
		store_itineraries_in_array(results->departures[d], leaves, &stored);
		for(int j = 0; j < stored; j++) {
			double total = stored_total_dv(leaves[j], calc_data, NULL);
			int depth = 0, max_hohmann = 0, max_synodic = 0, max_period = 0;
			for(struct ItinStep *step = leaves[j]; step->prev != NULL; step = step->prev) {
				depth++;
				if(step == leaves[j]) continue;	// final leg
				if(step->body == NULL || step->prev->body == NULL) continue;	// deep-space maneuver node
				// Resonant returns take whole revolutions, so they are not compared with the Hohmann time.
				if(step->body == step->prev->body) { resonant_legs++; continue; }
				double dt = step->date - step->prev->date;
				Hohmann hohmann = calc_hohmann_transfer(mag_vec3(step->prev->r), mag_vec3(step->r), system->cb);
				int bin = replay_ratio_bin(dt / (hohmann.dur / 86400));
				int synodic_bin = replay_ratio_bin(dt / replay_synodic_days(step->prev->body, step->body, system));
				int period_bin = replay_ratio_bin(dt / fmax(replay_period_days(step->prev->body, system), replay_period_days(step->body, system)));
				if(period_bin >= 4 && long_leg_examples < 8) {
					long_leg_examples++;
					printf("  long leg example: %s -> %s, %.1f days, Hohmann %.1f days, synodic %.1f days, total %.1f m/s\n",
						step->prev->body->name, step->body->name, dt, hohmann.dur / 86400,
						replay_synodic_days(step->prev->body, step->body, system), total);
				}
				hohmann_stats.legs[bin]++;
				synodic_stats.legs[synodic_bin]++;
				period_stats.legs[period_bin]++;
				if(bin > max_hohmann) max_hohmann = bin;
				if(synodic_bin > max_synodic) max_synodic = synodic_bin;
				if(period_bin > max_period) max_period = period_bin;
			}
			if(depth > REPLAY_MAX_DEPTH) depth = REPLAY_MAX_DEPTH;
			depth_counts[depth]++;
			if(isfinite(total)) {
				if(total < best_total) best_total = total;
				if(total < best_by_depth[depth]) best_by_depth[depth] = total;
				for(int i = depth; i <= REPLAY_MAX_DEPTH; i++) if(total < best_up_to_depth[i]) best_up_to_depth[i] = total;
				replay_ratio_stats_add_route(&hohmann_stats, max_hohmann, total);
				replay_ratio_stats_add_route(&synodic_stats, max_synodic, total);
				replay_ratio_stats_add_route(&period_stats, max_period, total);
			}
		}
		free(leaves);
	}
	printf("Best total delta-v: %.1f m/s; %lld resonant legs\n", best_total, resonant_legs);
	replay_ratio_stats_print(&hohmann_stats);
	replay_ratio_stats_print(&synodic_stats);
	replay_ratio_stats_print(&period_stats);
	printf("Routes by number of legs:\n");
	for(int depth = 1; depth <= REPLAY_MAX_DEPTH; depth++)
		if(depth_counts[depth] > 0) printf("  %d legs: %lld routes, best %.1f m/s, best with at most this many legs %.1f m/s\n",
			depth, depth_counts[depth], best_by_depth[depth], best_up_to_depth[depth]);
}

// Re-runs the search saved in an .itins file. A duration and a departure count override the saved ones, so a slow
// search can be measured on a few departures, and a flyby count overrides the route limit. Prints the saved settings first.
int run_itins_replay(const char *path, double max_duration, int num_departures, int max_flybys) {
	FILE *file = fopen(path, "rb");
	if(file == NULL) {
		fprintf(stderr, "Cannot open %s\n", path);
		return EXIT_FAILURE;
	}
	ItinStepBinHeaderData header = get_itins_bfile_header(file);
	fclose(file);
	if(header.num_deps < 0 || header.calc_data.seq_info.to_target.type != ITIN_SEQ_INFO_TO_TARGET) {
		fprintf(stderr, "%s is not a target search file\n", path);
		return EXIT_FAILURE;
	}
	Itin_Calc_Data calc_data = header.calc_data;
	struct ItinSequenceInfoToTarget *target = &calc_data.seq_info.to_target;
	printf("Saved: dep JD %.2f..%.2f step %.3f, max arr JD %.2f, max duration %.2f, dv dep/sat/tot %.0f/%.0f/%.0f, "
		"periapsis %.0f/%.0f, type %d, %d itineraries\n", calc_data.jd_min_dep, calc_data.jd_max_dep,
		calc_data.step_dep_date, calc_data.jd_max_arr, calc_data.max_duration, calc_data.dv_filter.max_depdv,
		calc_data.dv_filter.max_satdv, calc_data.dv_filter.max_totdv, calc_data.dv_filter.dep_periapsis,
		calc_data.dv_filter.arr_periapsis, calc_data.dv_filter.last_transfer_type, header.num_itins);
	printf("Saved bodies: %s -> %s via", target->dep_body->name, target->arr_body->name);
	for(int i = 0; i < target->num_flyby_bodies; i++) printf(" %s", target->flyby_bodies[i]->name);
	printf("\n");
	if(max_duration > 0) {
		calc_data.jd_max_arr += max_duration - calc_data.max_duration;
		calc_data.max_duration = max_duration;
	}
	if(calc_data.step_dep_date <= 0) calc_data.step_dep_date = 1;
	if(num_departures > 0) calc_data.jd_max_dep = calc_data.jd_min_dep + (num_departures - 1) * calc_data.step_dep_date;
	if(max_flybys > 0) set_max_flybys_per_route(max_flybys);
	if(benchmark_unpowered) calc_data.disable_powered_flybys = 1;
	if(benchmark_resonant) target->allow_resonant = 1;
	printf("Options: powered flybys %s, resonant %s, DSM %d, moon-aware %d, last transfer %d, max flybys %d\n",
		calc_data.disable_powered_flybys ? "off" : "on", target->allow_resonant ? "on" : "off",
		calc_data.allow_dsm, calc_data.moon_aware_flybys, calc_data.dv_filter.last_transfer_type, get_max_flybys_per_route());

	Itin_Calc_Results results = search_for_itineraries(calc_data);
	printf("Replay result: %d departure roots, %d nodes, %d itineraries\n",
		results.num_deps, results.num_nodes, results.num_itins);
	report_replay_route_statistics(&results, &calc_data);
	for(int i = 0; i < results.num_deps; i++) free_itinerary(results.departures[i]);
	free(results.departures);
	return EXIT_SUCCESS;
}
