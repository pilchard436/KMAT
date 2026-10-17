#include "powered_route.h"
#include "search_cancel.h"
#include <stdlib.h>
#include <string.h>

/* The SOI-converged objective is only repeatable to a few m/s, so smaller DSM gains are noise. */
#define POWERED_DSM_MIN_SAVING 5.0
#define POWERED_DSM_MIN_MANEUVER 1.0

static int dsm_search_frame(Vector3 position, Vector3 velocity, Vector3 axes[3]) {
	double speed = mag_vec3(velocity);
	Vector3 normal = cross_vec3(position, velocity);
	double normal_magnitude = mag_vec3(normal);
	if(!(speed > 0) || !(normal_magnitude > 0)) return 0;
	axes[0] = scale_vec3(velocity, 1 / speed);
	axes[2] = scale_vec3(normal, 1 / normal_magnitude);
	axes[1] = cross_vec3(axes[2], axes[0]);
	return 1;
}

/* Coordinate search over one DSM per leg on a route whose dates are fixed. Needs best_states,
 * best_legs and best_total_dv of the plain route; a leg keeps its DSM only if it lowers the cost. */
double optimize_candidate_dsm(PoweredRouteCandidate *candidate, const Itin_Calc_Data *calc_data) {
	static const double delta_steps[] = {300, 100, 30, 10, 3, 1};
	static const double eta_steps[] = {0.2, 0.1, 0.05, 0.03, 0.02, 0.01};
	size_t n = candidate->num_steps;
	OSV *trial_states = calloc(n, sizeof(*trial_states));
	Lambert3 *trial_legs = calloc(n - 1, sizeof(*trial_legs));
	PoweredDsmResult *trial_dsm = calloc(n - 1, sizeof(*trial_dsm));
	PoweredLegDsm *dsms = calloc(n - 1, sizeof(*dsms));
	PoweredDsmResult *dsm_results = calloc(n - 1, sizeof(*dsm_results));
	double best = candidate->best_total_dv;
	if(trial_states == NULL || trial_legs == NULL || trial_dsm == NULL || dsms == NULL || dsm_results == NULL)
		goto cleanup;

	candidate->dsms = dsms;
	OSV *leg_states = malloc(n * sizeof(*leg_states));
	Lambert3 *leg_legs = malloc((n - 1) * sizeof(*leg_legs));
	PoweredDsmResult *leg_dsm = malloc((n - 1) * sizeof(*leg_dsm));
	if(leg_states == NULL || leg_legs == NULL || leg_dsm == NULL) {
		free(leg_states); free(leg_legs); free(leg_dsm);
		candidate->dsms = NULL;
		goto cleanup;
	}
	for(size_t leg = 0; leg + 1 < n && objective_budget != 0 && !itinerary_search_cancelled(NULL); leg++) {
		Vector3 axes[3];
		if(!dsm_search_frame(candidate->best_states[leg].r, candidate->best_legs[leg].v0, axes)) continue;
		double cost_before_leg = best;
		memcpy(leg_states, candidate->best_states, n * sizeof(*leg_states));
		memcpy(leg_legs, candidate->best_legs, (n - 1) * sizeof(*leg_legs));
		memcpy(leg_dsm, dsm_results, (n - 1) * sizeof(*leg_dsm));
		dsms[leg] = (PoweredLegDsm){.enabled = 1, .eta = 0.5};
		for(size_t s = 0; s < sizeof(delta_steps) / sizeof(delta_steps[0]) &&
			objective_budget != 0 && !itinerary_search_cancelled(NULL); s++) {
			for(int pass = 0; pass < 4 && objective_budget != 0; pass++) {
				int improved = 0;
				for(int move = 0; move < 8 && objective_budget != 0; move++) {
					PoweredLegDsm saved = dsms[leg];
					int direction = move % 2 ? 1 : -1;
					if(move < 6) dsms[leg].delta_v = add_vec3(saved.delta_v,
						scale_vec3(axes[move / 2], direction * delta_steps[s]));
					else dsms[leg].eta = saved.eta + direction * eta_steps[s];
					if(dsms[leg].eta < 0.02 || dsms[leg].eta > 0.98) {
						dsms[leg] = saved;
						continue;
					}
					double value = evaluate_route_core(candidate, calc_data, candidate->times,
						trial_states, trial_legs, trial_dsm);
					if(value + POWERED_IMPROVEMENT_THRESHOLD < best) {
						best = value;
						improved = 1;
						memcpy(candidate->best_states, trial_states, n * sizeof(*trial_states));
						memcpy(candidate->best_legs, trial_legs, (n - 1) * sizeof(*trial_legs));
						memcpy(dsm_results, trial_dsm, (n - 1) * sizeof(*trial_dsm));
					} else {
						dsms[leg] = saved;
					}
				}
				if(!improved) break;
			}
		}
		int kept = best + POWERED_DSM_MIN_SAVING < cost_before_leg && dsm_results[leg].used &&
			dsm_results[leg].delta_v >= POWERED_DSM_MIN_MANEUVER;
		if(!kept) {
			dsms[leg].enabled = 0;
			best = cost_before_leg;
			memcpy(candidate->best_states, leg_states, n * sizeof(*leg_states));
			memcpy(candidate->best_legs, leg_legs, (n - 1) * sizeof(*leg_legs));
			memcpy(dsm_results, leg_dsm, (n - 1) * sizeof(*leg_dsm));
		}
	}
	free(leg_states); free(leg_legs); free(leg_dsm);

	if(best + POWERED_IMPROVEMENT_THRESHOLD < candidate->best_total_dv) {
		candidate->best_total_dv = best;
		candidate->dsm_results = dsm_results;
		dsm_results = NULL;
		dsms = NULL;
	} else {
		candidate->dsms = NULL;
		best = candidate->best_total_dv;
	}

cleanup:
	free(trial_states);
	free(trial_legs);
	free(trial_dsm);
	free(dsms);
	free(dsm_results);
	return best;
}

double optimize_powered_flyby_route_dsm(const Itin_Calc_Data *calc_data, Body **bodies,
		const double *times, size_t num_steps, PoweredLegDsm *dsms) {
	if(calc_data == NULL || bodies == NULL || times == NULL || dsms == NULL || num_steps < 3) return DBL_MAX;
	for(size_t i = 0; i < num_steps; i++)
		if(bodies[i] == NULL || !isfinite(times[i])) return DBL_MAX;
	memset(dsms, 0, (num_steps - 1) * sizeof(*dsms));
	PoweredRouteCandidate candidate = {.num_steps = num_steps, .bodies = bodies};
	candidate.times = malloc(num_steps * sizeof(*candidate.times));
	candidate.best_states = calloc(num_steps, sizeof(*candidate.best_states));
	candidate.best_legs = calloc(num_steps - 1, sizeof(*candidate.best_legs));
	double result = DBL_MAX;
	if(candidate.times != NULL && candidate.best_states != NULL && candidate.best_legs != NULL) {
		memcpy(candidate.times, times, num_steps * sizeof(*times));
		candidate.best_total_dv = evaluate_route(&candidate, calc_data, candidate.times,
			candidate.best_states, candidate.best_legs);
		result = candidate.best_total_dv;
		if(result < DBL_MAX) {
			objective_budget = POWERED_DSM_OBJECTIVES_PER_ROUTE;
			result = optimize_candidate_dsm(&candidate, calc_data);
			objective_budget = -1;
			if(candidate.dsms != NULL) memcpy(dsms, candidate.dsms, (num_steps - 1) * sizeof(*dsms));
		}
	}
	free(candidate.times);
	free(candidate.best_states);
	free(candidate.best_legs);
	free(candidate.dsms);
	free(candidate.dsm_results);
	return result;
}

int optimize_itinerary_maneuvers(CelestSystem *system, struct ItinStep *first,
double dep_periapsis, double arr_periapsis, enum LastTransferType last_transfer_type) {
if(system == NULL || first == NULL || first->prev != NULL || first->body == NULL) return 0;
size_t num_steps = 0;
struct ItinStep *last = first;
for(struct ItinStep *node = first; node != NULL; node = node->next != NULL ? node->next[0] : NULL) {
if(node->num_next_nodes > 1) return 0;
num_steps += node->body != NULL;
last = node;
}
if(num_steps < 3 || last->body == NULL) return 0;

Body **bodies = malloc(num_steps * sizeof(*bodies));
double *times = malloc(num_steps * sizeof(*times));
PoweredRouteCandidate candidate = {.num_steps = num_steps, .bodies = bodies, .times = times};
candidate.best_states = calloc(num_steps, sizeof(*candidate.best_states));
candidate.best_legs = calloc(num_steps - 1, sizeof(*candidate.best_legs));
int success = 0;
if(bodies == NULL || times == NULL || candidate.best_states == NULL || candidate.best_legs == NULL) goto cleanup;

size_t route_index = 0;
for(struct ItinStep *node = first; node != NULL; node = node->next != NULL ? node->next[0] : NULL) {
if(node->body == NULL) continue;
bodies[route_index] = node->body;
times[route_index] = node->date;
route_index++;
}
for(size_t i = 0; i + 1 < num_steps; i++)
if(!(times[i + 1] > times[i])) goto cleanup;

Itin_Calc_Data calc_data = {0};
calc_data.jd_max_arr = times[num_steps - 1] + 1;
calc_data.max_duration = times[num_steps - 1] - times[0] + 1;
calc_data.dv_filter = (struct Dv_Filter){.max_totdv = 1e12, .max_depdv = 1e12, .max_satdv = 1e12,
.dep_periapsis = dep_periapsis, .arr_periapsis = arr_periapsis, .last_transfer_type = last_transfer_type};
calc_data.seq_info.to_target.system = system;

candidate.best_total_dv = evaluate_route(&candidate, &calc_data, times, candidate.best_states, candidate.best_legs);
if(candidate.best_total_dv >= DBL_MAX) goto cleanup;
objective_budget = POWERED_DSM_OBJECTIVES_PER_ROUTE;
optimize_candidate_dsm(&candidate, &calc_data);
objective_budget = -1;

// The old maneuver nodes are replaced by the optimized ones; the first node stays.
struct ItinStep *suffix = make_optimized_suffix(&candidate);
if(suffix != NULL) {
struct ItinStep *old = first->next[0];
first->next[0] = suffix;
suffix->prev = first;
old->prev = NULL;
free_itinerary(old);
success = 1;
}

cleanup:
free(bodies);
free(times);
free(candidate.best_states);
free(candidate.best_legs);
free(candidate.dsms);
free(candidate.dsm_results);
return success;
}

