#include "integration_common.h"

#include <time.h>

#define COVERAGE_GRID_STEP_DAYS 4.0
#define COVERAGE_NEAR_OPTIMAL 0.50
#define COVERAGE_PROMINENCE 0.03
#define COVERAGE_BASIN_DAYS 8.0
#define COVERAGE_TIGHT 0.02
#define COVERAGE_REPORTED_FAMILIES 15

typedef struct CoverageRoute {
	double offset[3], objective;
} CoverageRoute;

static int compare_coverage_routes(const void *left, const void *right) {
	double a = ((const CoverageRoute *)left)->objective;
	double b = ((const CoverageRoute *)right)->objective;
	return (a > b) - (a < b);
}

static const double *coverage_sort_grid;

static int compare_grid_cells(const void *left, const void *right) {
	double a = coverage_sort_grid[*(const int *)left];
	double b = coverage_sort_grid[*(const int *)right];
	return (a > b) - (a < b);
}

static CoverageRoute coverage_cell_route(int cell, const int dims[3], double step, double objective) {
	int i = cell / (dims[1] * dims[2]), j = (cell / dims[2]) % dims[1], k = cell % dims[2];
	return (CoverageRoute){.offset = {POWERED_TEST_MIN_LEG_DAYS + i * step,
		POWERED_TEST_MIN_LEG_DAYS + j * step, POWERED_TEST_MIN_LEG_DAYS + k * step}, .objective = objective};
}

// A family is a valley of the objective surface: two minima stay separate only if the saddle
// between them rises at least COVERAGE_PROMINENCE above the shallower valley floor.
// Cell index is (i * dims[1] + j) * dims[2] + k; returns the family count, or -1 on allocation failure.
static int find_objective_valleys(const double *grid, const int dims[3], double step,
		double near_optimal_limit, CoverageRoute *out) {
	int cells = dims[0] * dims[1] * dims[2];
	int *order = malloc((size_t)cells * sizeof(*order));
	int *parent = malloc((size_t)cells * sizeof(*parent));
	char *seen = calloc((size_t)cells, 1);
	if(order == NULL || parent == NULL || seen == NULL) {
		free(order); free(parent); free(seen);
		return -1;
	}
	int ordered = 0, count = 0;
	for(int c = 0; c < cells; c++) {
		parent[c] = c;
		if(grid[c] < DBL_MAX) order[ordered++] = c;
	}
	coverage_sort_grid = grid;
	qsort(order, (size_t)ordered, sizeof(*order), compare_grid_cells);
	for(int n = 0; n < ordered; n++) {
		int cell = order[n];
		int ci = cell / (dims[1] * dims[2]), cj = (cell / dims[2]) % dims[1], ck = cell % dims[2];
		int roots[26], root_count = 0;
		for(int di = -1; di <= 1; di++)
			for(int dj = -1; dj <= 1; dj++)
				for(int dk = -1; dk <= 1; dk++) {
					int ni = ci + di, nj = cj + dj, nk = ck + dk;
					if((di == 0 && dj == 0 && dk == 0) || ni < 0 || nj < 0 || nk < 0 ||
						ni >= dims[0] || nj >= dims[1] || nk >= dims[2]) continue;
					int neighbour = (ni * dims[1] + nj) * dims[2] + nk;
					if(!seen[neighbour]) continue;
					int root = neighbour;
					while(parent[root] != root) {
						parent[root] = parent[parent[root]];
						root = parent[root];
					}
					int known = 0;
					for(int r = 0; r < root_count; r++) known |= roots[r] == root;
					if(!known) roots[root_count++] = root;
				}
		seen[cell] = 1;
		if(root_count == 0) continue;
		int oldest = roots[0];
		for(int r = 1; r < root_count; r++)
			if(grid[roots[r]] < grid[oldest]) oldest = roots[r];
		parent[cell] = oldest;
		for(int r = 0; r < root_count; r++) {
			if(roots[r] == oldest) continue;
			double floor_value = grid[roots[r]];
			if(grid[cell] - floor_value >= COVERAGE_PROMINENCE * floor_value && floor_value <= near_optimal_limit)
				out[count++] = coverage_cell_route(roots[r], dims, step, floor_value);
			parent[roots[r]] = oldest;
		}
	}
	for(int c = 0; c < cells; c++)
		if(seen[c] && parent[c] == c && grid[c] <= near_optimal_limit)
			out[count++] = coverage_cell_route(c, dims, step, grid[c]);
	free(order); free(parent); free(seen);
	qsort(out, (size_t)count, sizeof(*out), compare_coverage_routes);
	return count;
}

// Informational: compares the search with a dense grid of the exact objective on one small mission.
int run_coverage_reference(CelestSystem *system, Body *departure_body, Body *flyby_body,
		Body *arrival_body, double departure_jd, double hohmann_duration) {
	Body *flyby_bodies[] = {flyby_body, arrival_body};
	Itin_Calc_Data calc_data = {0};
	calc_data.jd_min_dep = departure_jd;
	calc_data.jd_max_dep = departure_jd;
	calc_data.jd_max_arr = departure_jd + 1.5 * hohmann_duration;
	calc_data.max_duration = calc_data.jd_max_arr - departure_jd;
	calc_data.dv_filter = (struct Dv_Filter) {
		.max_totdv = 1e9, .max_depdv = 1e9, .max_satdv = 1e9,
		.dep_periapsis = departure_body->atmo_alt + 50000,
		.arr_periapsis = arrival_body->atmo_alt + 50000,
		.last_transfer_type = TF_FLYBY
	};
	calc_data.seq_info.to_target = (struct ItinSequenceInfoToTarget) {
		.type = ITIN_SEQ_INFO_TO_TARGET, .system = system, .dep_body = departure_body,
		.arr_body = arrival_body, .flyby_bodies = flyby_bodies, .num_flyby_bodies = 2
	};
	Itin_Calc_Results results = search_for_itineraries(calc_data);

	int exit_code = EXIT_FAILURE;
	CoverageRoute *found = NULL, *minima = NULL;
	double *grid = NULL;
	int found_count = 0, minima_count = 0;
	int total_itineraries = 0;
	for(int i = 0; i < results.num_deps; i++)
		total_itineraries += get_number_of_itineraries(results.departures[i]);
	found = malloc((size_t)(total_itineraries > 0 ? total_itineraries : 1) * sizeof(*found));
	struct ItinStep **arrivals = malloc((size_t)(total_itineraries > 0 ? total_itineraries : 1) * sizeof(*arrivals));
	if(found == NULL || arrivals == NULL) { free(arrivals); goto cleanup; }
	for(int i = 0; i < results.num_deps; i++) {
		int stored = 0;
		store_itineraries_in_array(results.departures[i], arrivals, &stored);
		for(int j = 0; j < stored; j++) {
			struct ItinStep *leaf = arrivals[j];
			if(leaf->prev == NULL || leaf->prev->prev == NULL || leaf->prev->prev->prev != NULL ||
				leaf->prev->body != flyby_body) continue;
			double objective = score_powered_arrival_route(leaf, &calc_data, flyby_body);
			if(!isfinite(objective) || objective >= DBL_MAX) continue;
			found[found_count++] = (CoverageRoute){.offset = {leaf->prev->date - departure_jd,
				leaf->date - departure_jd}, .objective = objective};
		}
	}
	free(arrivals);

	const double step = COVERAGE_GRID_STEP_DAYS;
	int n = (int)floor((calc_data.max_duration - POWERED_TEST_MIN_LEG_DAYS) / step) + 1;
	grid = malloc((size_t)n * n * sizeof(*grid));
	if(n <= 0 || grid == NULL) goto cleanup;
	Body *bodies[] = {departure_body, flyby_body, arrival_body};
	unsigned long long evaluations = 0;
	int feasible_cells = 0;
	double reference_best = DBL_MAX;
	for(int i = 0; i < n; i++) {
		for(int j = 0; j < n; j++) {
			double flyby_offset = POWERED_TEST_MIN_LEG_DAYS + i * step;
			double arrival_offset = POWERED_TEST_MIN_LEG_DAYS + j * step;
			grid[(size_t)i * n + j] = DBL_MAX;
			if(arrival_offset < flyby_offset + POWERED_TEST_MIN_LEG_DAYS ||
				arrival_offset > calc_data.max_duration) continue;
			double times[] = {departure_jd, departure_jd + flyby_offset, departure_jd + arrival_offset};
			double objective = evaluate_powered_flyby_route_objective(&calc_data, bodies, times, 3);
			evaluations++;
			if(!isfinite(objective) || objective >= DBL_MAX) continue;
			grid[(size_t)i * n + j] = objective;
			feasible_cells++;
			if(objective < reference_best) reference_best = objective;
		}
	}
	if(feasible_cells == 0) goto cleanup;

	// Grid cells are (flyby, arrival) pairs; the shared function handles the valley analysis.
	int dims[3] = {n, n, 1};
	minima = malloc((size_t)feasible_cells * sizeof(*minima));
	if(minima == NULL) goto cleanup;
	double near_optimal_limit = reference_best * (1 + COVERAGE_NEAR_OPTIMAL);
	minima_count = find_objective_valleys(grid, dims, step, near_optimal_limit, minima);
	if(minima_count < 0) { minima_count = 0; goto cleanup; }
	double search_best = DBL_MAX;
	for(int i = 0; i < found_count; i++)
		if(found[i].objective < search_best) search_best = found[i].objective;
	printf("Coverage reference (Earth-Venus-Jupiter, one departure): grid step %.1f d, %llu evaluations, %d feasible cells\n",
		step, evaluations, feasible_cells);
	printf("Reference best %.1f m/s; search returned %d routes of this sequence, best %.1f m/s\n",
		reference_best, found_count, search_best);
	printf("Powered seeds added %llu, encounter searches skipped after budget %llu\n",
		results.powered_seed_stats.seeds_added, results.powered_seed_stats.searches_budget_skipped);
	{
		// Mirrors the first-leg duration window used by the search (calc_itins_from_departure).
		OSV dep_state = osv_from_elements(departure_body->orbit, departure_jd);
		OSV flyby_state = osv_from_elements(flyby_body->orbit, departure_jd);
		double r0 = mag_vec3(dep_state.r), r1 = mag_vec3(flyby_state.r);
		double hohmann_days = calc_hohmann_transfer(r0, r1, system->cb).dur / 86400;
		double ratio = r1 / r0;
		double window_days = (4 * (ratio - 0.85) * (ratio - 0.85) + 1.5) * hohmann_days;
		if(window_days / hohmann_days > 3) window_days = 3 * hohmann_days;
		printf("First-leg grid to %s covers %.0f to %.0f days after departure\n",
			flyby_body->name, 0.4 * hohmann_days, window_days);
	}
	int in_basin = 0, tight = 0;
	for(int i = 0; i < minima_count; i++) {
		double best_in_basin = DBL_MAX;
		int routes_in_basin = 0;
		for(int k = 0; k < found_count; k++) {
			if(fabs(found[k].offset[0] - minima[i].offset[0]) > COVERAGE_BASIN_DAYS ||
				fabs(found[k].offset[1] - minima[i].offset[1]) > COVERAGE_BASIN_DAYS) continue;
			routes_in_basin++;
			if(found[k].objective < best_in_basin) best_in_basin = found[k].objective;
		}
		int covered = routes_in_basin > 0;
		int is_tight = covered && best_in_basin <= minima[i].objective * (1 + COVERAGE_TIGHT);
		in_basin += covered;
		tight += is_tight;
		if(i < COVERAGE_REPORTED_FAMILIES) {
			if(covered)
				printf("  family %2d: flyby +%4.0f d, arrival +%4.0f d, reference %.1f m/s, search %.1f m/s (%d routes) %s\n",
					i + 1, minima[i].offset[0], minima[i].offset[1], minima[i].objective,
					best_in_basin, routes_in_basin, is_tight ? "COVERED" : "found, not refined to within 2%");
			else
				printf("  family %2d: flyby +%4.0f d, arrival +%4.0f d, reference %.1f m/s, search returned none  MISSED\n",
					i + 1, minima[i].offset[0], minima[i].offset[1], minima[i].objective);
		}
	}
	printf("Families within %.0f%% of the reference best: %d; basin covered %d/%d; within %.0f%% of reference %d/%d\n",
		COVERAGE_NEAR_OPTIMAL * 100, minima_count, in_basin, minima_count,
		COVERAGE_TIGHT * 100, tight, minima_count);
	exit_code = EXIT_SUCCESS;

cleanup:
	free(found);
	free(grid);
	free(minima);
	for(int i = 0; i < results.num_deps; i++) free_itinerary(results.departures[i]);
	free(results.departures);
	return exit_code;
}

// Local coordinate descent on the exact objective; offsets are days after departure, one per encounter after the first.
static double refine_reference_offsets(const Itin_Calc_Data *calc_data, Body **bodies, int num_steps,
		double departure_jd, double *offsets, double objective) {
	static const double steps[] = {8, 4, 2, 1, 0.5};
	int free_dates = num_steps - 1;
	double times[8] = {departure_jd};
	for(size_t s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
		for(int pass = 0; pass < 200; pass++) {
			int improved = 0;
			for(int d = 0; d < free_dates; d++) {
				for(int direction = -1; direction <= 1; direction += 2) {
					double trial = offsets[d] + direction * steps[s];
					double lower = (d == 0 ? 0 : offsets[d - 1]) + POWERED_TEST_MIN_LEG_DAYS;
					double upper = d + 1 < free_dates ? offsets[d + 1] - POWERED_TEST_MIN_LEG_DAYS :
						calc_data->max_duration;
					if(trial < lower || trial > upper) continue;
					double saved = offsets[d];
					offsets[d] = trial;
					for(int k = 0; k < free_dates; k++) times[k + 1] = departure_jd + offsets[k];
					double value = evaluate_powered_flyby_route_objective(calc_data, bodies,
						times, (size_t)num_steps);
					if(isfinite(value) && value < objective) {
						objective = value;
						improved = 1;
					} else {
						offsets[d] = saved;
					}
				}
			}
			if(!improved) break;
		}
	}
	return objective;
}

// Informational: Earth-Venus-Earth-Jupiter has three free dates, so the grid is coarse and its valleys are refined locally.
int run_multi_flyby_coverage(CelestSystem *system, Body *departure_body, Body *venus,
		Body *arrival_body, double departure_jd, double hohmann_duration) {
	const double coarse_step = 15.0;
	Body *flyby_bodies[] = {venus, departure_body, arrival_body};
	Body *sequence[] = {departure_body, venus, departure_body, arrival_body};
	Itin_Calc_Data calc_data = {0};
	calc_data.jd_min_dep = departure_jd;
	calc_data.jd_max_dep = departure_jd;
	calc_data.jd_max_arr = departure_jd + 1.5 * hohmann_duration;
	calc_data.max_duration = calc_data.jd_max_arr - departure_jd;
	calc_data.dv_filter = (struct Dv_Filter) {
		.max_totdv = 1e9, .max_depdv = 1e9, .max_satdv = 1e9,
		.dep_periapsis = departure_body->atmo_alt + 50000,
		.arr_periapsis = arrival_body->atmo_alt + 50000,
		.last_transfer_type = TF_FLYBY
	};
	calc_data.seq_info.to_target = (struct ItinSequenceInfoToTarget) {
		.type = ITIN_SEQ_INFO_TO_TARGET, .system = system, .dep_body = departure_body,
		.arr_body = arrival_body, .flyby_bodies = flyby_bodies, .num_flyby_bodies = 3
	};
	time_t search_started = time(NULL);
	Itin_Calc_Results results = search_for_itineraries(calc_data);
	double search_seconds = difftime(time(NULL), search_started);

	int exit_code = EXIT_FAILURE;
	CoverageRoute *found = NULL, *valleys = NULL, *families = NULL;
	double *grid = NULL;
	int found_count = 0, family_count = 0;
	int total_itineraries = 0;
	for(int i = 0; i < results.num_deps; i++)
		total_itineraries += get_number_of_itineraries(results.departures[i]);
	found = malloc((size_t)(total_itineraries > 0 ? total_itineraries : 1) * sizeof(*found));
	struct ItinStep **arrivals = malloc((size_t)(total_itineraries > 0 ? total_itineraries : 1) * sizeof(*arrivals));
	if(found == NULL || arrivals == NULL) { free(arrivals); goto cleanup; }
	for(int i = 0; i < results.num_deps; i++) {
		int stored = 0;
		store_itineraries_in_array(results.departures[i], arrivals, &stored);
		for(int j = 0; j < stored; j++) {
			struct ItinStep *route[4] = {arrivals[j]};
			int length = 1;
			for(struct ItinStep *step = arrivals[j]->prev; step != NULL && length < 5; step = step->prev)
				if(length < 4) route[length++] = step; else length++;
			if(length != 4 || route[0]->body != sequence[3] || route[1]->body != sequence[2] ||
				route[2]->body != sequence[1] || route[3]->body != sequence[0]) continue;
			double objective = score_powered_arrival_route(arrivals[j], &calc_data, venus);
			if(!isfinite(objective) || objective >= DBL_MAX) continue;
			found[found_count++] = (CoverageRoute){.offset = {route[2]->date - departure_jd,
				route[1]->date - departure_jd, route[0]->date - departure_jd}, .objective = objective};
		}
	}
	free(arrivals);

	int n = (int)floor((calc_data.max_duration - POWERED_TEST_MIN_LEG_DAYS) / coarse_step) + 1;
	if(n <= 0) goto cleanup;
	size_t cells = (size_t)n * n * n;
	grid = malloc(cells * sizeof(*grid));
	if(grid == NULL) goto cleanup;
	unsigned long long evaluations = 0;
	int feasible_cells = 0;
	double coarse_best = DBL_MAX;
	for(size_t c = 0; c < cells; c++) grid[c] = DBL_MAX;
	for(int i = 0; i < n; i++) {
		for(int j = i; j < n; j++) {
			for(int k = j; k < n; k++) {
				double offsets[3] = {POWERED_TEST_MIN_LEG_DAYS + i * coarse_step,
					POWERED_TEST_MIN_LEG_DAYS + j * coarse_step, POWERED_TEST_MIN_LEG_DAYS + k * coarse_step};
				if(offsets[1] < offsets[0] + POWERED_TEST_MIN_LEG_DAYS ||
					offsets[2] < offsets[1] + POWERED_TEST_MIN_LEG_DAYS ||
					offsets[2] > calc_data.max_duration) continue;
				double times[] = {departure_jd, departure_jd + offsets[0], departure_jd + offsets[1],
					departure_jd + offsets[2]};
				double objective = evaluate_powered_flyby_route_objective(&calc_data, sequence, times, 4);
				evaluations++;
				if(!isfinite(objective) || objective >= DBL_MAX) continue;
				grid[((size_t)i * n + j) * n + k] = objective;
				feasible_cells++;
				if(objective < coarse_best) coarse_best = objective;
			}
		}
	}
	if(feasible_cells == 0) goto cleanup;

	valleys = malloc((size_t)feasible_cells * sizeof(*valleys));
	if(valleys == NULL) goto cleanup;
	int dims[3] = {n, n, n};
	int valley_count = find_objective_valleys(grid, dims, coarse_step, coarse_best * (1 + COVERAGE_NEAR_OPTIMAL), valleys);
	if(valley_count < 0) goto cleanup;
	families = malloc((size_t)(valley_count > 0 ? valley_count : 1) * sizeof(*families));
	if(families == NULL) goto cleanup;
	double reference_best = DBL_MAX;
	for(int v = 0; v < valley_count; v++) {
		CoverageRoute refined = valleys[v];
		refined.objective = refine_reference_offsets(&calc_data, sequence, 4, departure_jd,
			refined.offset, refined.objective);
		if(refined.objective < reference_best) reference_best = refined.objective;
		valleys[v] = refined;
	}
	qsort(valleys, (size_t)valley_count, sizeof(*valleys), compare_coverage_routes);
	for(int v = 0; v < valley_count; v++) {
		if(valleys[v].objective > reference_best * (1 + COVERAGE_NEAR_OPTIMAL)) continue;
		int duplicate = 0;
		for(int f = 0; f < family_count && !duplicate; f++)
			duplicate = fabs(valleys[v].offset[0] - families[f].offset[0]) <= COVERAGE_BASIN_DAYS &&
				fabs(valleys[v].offset[1] - families[f].offset[1]) <= COVERAGE_BASIN_DAYS &&
				fabs(valleys[v].offset[2] - families[f].offset[2]) <= COVERAGE_BASIN_DAYS;
		if(!duplicate) families[family_count++] = valleys[v];
	}

	double search_best = DBL_MAX;
	for(int i = 0; i < found_count; i++)
		if(found[i].objective < search_best) search_best = found[i].objective;
	printf("Multi-flyby coverage reference (Earth-Venus-Earth-Jupiter, one departure): coarse step %.0f d, %llu evaluations, %d feasible cells, %d valleys refined\n",
		coarse_step, evaluations, feasible_cells, valley_count);
	printf("Reference best %.1f m/s; search returned %d routes of this sequence, best %.1f m/s\n",
		reference_best, found_count, search_best);
	printf("Powered seeds added %llu, encounter searches skipped after budget %llu; search took %.0f s\n",
		results.powered_seed_stats.seeds_added, results.powered_seed_stats.searches_budget_skipped,
		search_seconds);
	int in_basin = 0, tight = 0;
	for(int i = 0; i < family_count; i++) {
		double best_in_basin = DBL_MAX;
		int routes_in_basin = 0;
		for(int k = 0; k < found_count; k++) {
			if(fabs(found[k].offset[0] - families[i].offset[0]) > COVERAGE_BASIN_DAYS ||
				fabs(found[k].offset[1] - families[i].offset[1]) > COVERAGE_BASIN_DAYS ||
				fabs(found[k].offset[2] - families[i].offset[2]) > COVERAGE_BASIN_DAYS) continue;
			routes_in_basin++;
			if(found[k].objective < best_in_basin) best_in_basin = found[k].objective;
		}
		int covered = routes_in_basin > 0;
		int is_tight = covered && best_in_basin <= families[i].objective * (1 + COVERAGE_TIGHT);
		in_basin += covered;
		tight += is_tight;
		if(i < COVERAGE_REPORTED_FAMILIES) {
			if(covered)
				printf("  family %2d: Venus +%4.0f d, Earth +%4.0f d, Jupiter +%4.0f d, reference %.1f m/s, search %.1f m/s (%d routes) %s\n",
					i + 1, families[i].offset[0], families[i].offset[1], families[i].offset[2],
					families[i].objective, best_in_basin, routes_in_basin,
					is_tight ? "COVERED" : "found, not refined to within 2%");
			else
				printf("  family %2d: Venus +%4.0f d, Earth +%4.0f d, Jupiter +%4.0f d, reference %.1f m/s, search returned none  MISSED\n",
					i + 1, families[i].offset[0], families[i].offset[1], families[i].offset[2],
					families[i].objective);
		}
	}
	printf("Families within %.0f%% of the reference best: %d; basin covered %d/%d; within %.0f%% of reference %d/%d\n",
		COVERAGE_NEAR_OPTIMAL * 100, family_count, in_basin, family_count,
		COVERAGE_TIGHT * 100, tight, family_count);
	exit_code = EXIT_SUCCESS;

cleanup:
	free(found);
	free(grid);
	free(valleys);
	free(families);
	for(int i = 0; i < results.num_deps; i++) free_itinerary(results.departures[i]);
	free(results.departures);
	return exit_code;
}
