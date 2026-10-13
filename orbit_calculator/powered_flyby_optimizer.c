#include "powered_flyby_optimizer.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define POWERED_ROUTE_LIMIT 100
#define POWERED_MIN_LEG_DAYS 10.0
#define POWERED_IMPROVEMENT_THRESHOLD 0.1

typedef struct PoweredRouteCandidate {
	int departure_index;
	size_t num_steps;
	size_t mutable_start;
	Body **bodies;
	double *signature_dates;
	Vector3 *signature_positions;
	Vector3 *signature_v_arr;
	Vector3 *signature_v_dep;
	double *times;
	OSV *best_states;
	Lambert3 *best_legs;
	double original_total_dv;
	double best_total_dv;
} PoweredRouteCandidate;

static int is_finite_vector(Vector3 value) {
	return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}

static double arrival_delta_v(const struct Dv_Filter *filter, Body *body,
		Vector3 arrival_velocity, Vector3 body_velocity, double periapsis_altitude) {
	double vinf = mag_vec3(subtract_vec3(arrival_velocity, body_velocity));
	if(filter->last_transfer_type == TF_CAPTURE)
		return dv_capture(body, alt2radius(body, periapsis_altitude), vinf);
	if(filter->last_transfer_type == TF_CIRC)
		return dv_circ(body, alt2radius(body, periapsis_altitude), vinf);
	return 0;
}

static int body_state_at(CelestSystem *system, Body *body, double epoch, OSV *state) {
	*state = system->prop_method == ORB_ELEMENTS ?
		osv_from_elements(body->orbit, epoch) :
		osv_from_ephem(body->ephem, body->num_ephems, epoch, system->cb);
	return is_finite_vector(state->r) && is_finite_vector(state->v);
}

static int hyperbolic_excess_velocity(OSV relative_state, Body *body, int incoming,
		Vector3 *excess_velocity) {
	double radius = mag_vec3(relative_state.r);
	Vector3 angular_momentum = cross_vec3(relative_state.r, relative_state.v);
	double angular_momentum_magnitude = mag_vec3(angular_momentum);
	if(!(radius > 0) || !(angular_momentum_magnitude > 0) || !(body->mu > 0)) return 0;
	double specific_energy = 0.5 * sq_mag_vec3(relative_state.v) - body->mu / radius;
	if(!(specific_energy > 0)) return 0;
	Vector3 eccentricity_vector = subtract_vec3(
		scale_vec3(cross_vec3(relative_state.v, angular_momentum), 1 / body->mu),
		scale_vec3(relative_state.r, 1 / radius));
	double eccentricity = mag_vec3(eccentricity_vector);
	if(!(eccentricity > 1)) return 0;
	double semi_latus_rectum = sq_mag_vec3(angular_momentum) / body->mu;
	if(!(semi_latus_rectum > 0)) return 0;

	Vector3 periapsis_direction = scale_vec3(eccentricity_vector, 1 / eccentricity);
	Vector3 orbit_normal = scale_vec3(angular_momentum, 1 / angular_momentum_magnitude);
	Vector3 transverse_direction = norm_vec3(cross_vec3(orbit_normal, periapsis_direction));
	double asymptote_anomaly = acos(-1 / eccentricity);
	double sine_anomaly = sin(asymptote_anomaly) * (incoming ? -1 : 1);
	double cosine_anomaly = -1 / eccentricity;
	double velocity_scale = sqrt(body->mu / semi_latus_rectum);
	*excess_velocity = add_vec3(
		scale_vec3(periapsis_direction, -velocity_scale * sine_anomaly),
		scale_vec3(transverse_direction, velocity_scale * (eccentricity + cosine_anomaly)));
	return is_finite_vector(*excess_velocity);
}

static double evaluate_route(const PoweredRouteCandidate *candidate, const Itin_Calc_Data *calc_data,
		const double *times, OSV *states, Lambert3 *legs) {
	CelestSystem *system = calc_data->seq_info.to_target.system;
	double latest_arrival = fmin(calc_data->jd_max_arr, times[0] + calc_data->max_duration);
	if(times[candidate->num_steps - 1] > latest_arrival) return DBL_MAX;

	for(size_t i = 0; i < candidate->num_steps; i++) {
		Body *body = candidate->bodies[i];
		if(!body_state_at(system, body, times[i], &states[i])) return DBL_MAX;
	}

	for(size_t i = 0; i + 1 < candidate->num_steps; i++) {
		double duration_days = times[i + 1] - times[i];
		if(duration_days < POWERED_MIN_LEG_DAYS) return DBL_MAX;
		legs[i] = calc_lambert3_selected(states[i].r, states[i + 1].r,
			duration_days * 86400, system->cb);
		if(legs[i].success == LAMBERT_FAIL_NAN || legs[i].success == LAMBERT_FAIL_ECC ||
			legs[i].success == LAMBERT_MAX_ITERATIONS || !is_finite_vector(legs[i].v0) ||
			!is_finite_vector(legs[i].v1)) return DBL_MAX;
	}

	PoweredFlybyResult *flybys = calloc(candidate->num_steps, sizeof(*flybys));
	Vector3 *incoming_excess = calloc(candidate->num_steps, sizeof(*incoming_excess));
	Vector3 *outgoing_excess = calloc(candidate->num_steps, sizeof(*outgoing_excess));
	if(flybys == NULL || incoming_excess == NULL || outgoing_excess == NULL) {
		free(flybys);
		free(incoming_excess);
		free(outgoing_excess);
		return DBL_MAX;
	}
	double total_dv = DBL_MAX;
	int valid_route = 1;
	for(size_t i = 1; i + 1 < candidate->num_steps; i++) {
		Body *body = candidate->bodies[i];
		double minimum_periapsis = body->radius + body->atmo_alt + 1e4;
		incoming_excess[i] = subtract_vec3(legs[i - 1].v1, states[i].v);
		outgoing_excess[i] = subtract_vec3(legs[i].v0, states[i].v);
		if(!calculate_powered_flyby(incoming_excess[i], outgoing_excess[i], vec3(0, 0, 0),
			body, minimum_periapsis, &flybys[i])) {
			valid_route = 0;
			break;
		}
	}

	int converged = 0;
	for(int iteration = 0; valid_route && iteration < 60; iteration++) {
		for(size_t i = 0; i + 1 < candidate->num_steps; i++) {
			double departure_epoch = times[i];
			double arrival_epoch = times[i + 1];
			Vector3 departure_position = states[i].r;
			Vector3 arrival_position = states[i + 1].r;
			if(i > 0) {
				departure_epoch += flybys[i].outgoing_soi_time / 86400;
				OSV body_at_exit;
				if(!body_state_at(system, candidate->bodies[i], departure_epoch, &body_at_exit)) {
					valid_route = 0;
					break;
				}
				departure_position = add_vec3(body_at_exit.r, flybys[i].outgoing_soi_state.r);
			}
			if(i + 1 < candidate->num_steps - 1) {
				arrival_epoch -= flybys[i + 1].incoming_soi_time / 86400;
				OSV body_at_entry;
				if(!body_state_at(system, candidate->bodies[i + 1], arrival_epoch, &body_at_entry)) {
					valid_route = 0;
					break;
				}
				arrival_position = add_vec3(body_at_entry.r, flybys[i + 1].incoming_soi_state.r);
			}
			double duration_days = arrival_epoch - departure_epoch;
			if(duration_days < POWERED_MIN_LEG_DAYS) {
				valid_route = 0;
				break;
			}
			legs[i] = calc_lambert3_selected(departure_position, arrival_position,
				duration_days * 86400, system->cb);
			if(legs[i].success == LAMBERT_FAIL_NAN || legs[i].success == LAMBERT_FAIL_ECC ||
				legs[i].success == LAMBERT_MAX_ITERATIONS || !is_finite_vector(legs[i].v0) ||
				!is_finite_vector(legs[i].v1)) {
				valid_route = 0;
				break;
			}
		}
		if(!valid_route) break;

		double maximum_time_change = 0;
		double maximum_velocity_change = 0;
		double maximum_boundary_position_change = 0;
		for(size_t i = 1; i + 1 < candidate->num_steps; i++) {
			Body *body = candidate->bodies[i];
			double entry_epoch = times[i] - flybys[i].incoming_soi_time / 86400;
			double exit_epoch = times[i] + flybys[i].outgoing_soi_time / 86400;
			OSV body_at_entry, body_at_exit;
			if(!body_state_at(system, body, entry_epoch, &body_at_entry) ||
				!body_state_at(system, body, exit_epoch, &body_at_exit)) {
				valid_route = 0;
				break;
			}
			OSV incoming_boundary = {
				subtract_vec3(legs[i - 1].r1, body_at_entry.r),
				subtract_vec3(legs[i - 1].v1, body_at_entry.v)
			};
			OSV outgoing_boundary = {
				subtract_vec3(legs[i].r0, body_at_exit.r),
				subtract_vec3(legs[i].v0, body_at_exit.v)
			};
			Vector3 new_incoming_excess, new_outgoing_excess;
			if(!hyperbolic_excess_velocity(incoming_boundary, body, 1, &new_incoming_excess) ||
				!hyperbolic_excess_velocity(outgoing_boundary, body, 0, &new_outgoing_excess)) {
				valid_route = 0;
				break;
			}
			new_incoming_excess = scale_vec3(add_vec3(incoming_excess[i], new_incoming_excess), 0.5);
			new_outgoing_excess = scale_vec3(add_vec3(outgoing_excess[i], new_outgoing_excess), 0.5);
			PoweredFlybyResult updated_flyby;
			double minimum_periapsis = body->radius + body->atmo_alt + 1e4;
			if(!calculate_powered_flyby(new_incoming_excess, new_outgoing_excess,
				vec3(0, 0, 0), body, minimum_periapsis, &updated_flyby)) {
				valid_route = 0;
				break;
			}
			double time_change = fmax(fabs(updated_flyby.incoming_soi_time - flybys[i].incoming_soi_time),
				fabs(updated_flyby.outgoing_soi_time - flybys[i].outgoing_soi_time));
			double time_scale = fmax(1, fmax(updated_flyby.incoming_soi_time,
				updated_flyby.outgoing_soi_time));
			maximum_time_change = fmax(maximum_time_change, time_change / time_scale);
			double velocity_change = fmax(mag_vec3(subtract_vec3(new_incoming_excess, incoming_excess[i])),
				mag_vec3(subtract_vec3(new_outgoing_excess, outgoing_excess[i])));
			double velocity_scale = fmax(1, fmax(mag_vec3(new_incoming_excess), mag_vec3(new_outgoing_excess)));
			double velocity_mismatch = fmax(
				mag_vec3(subtract_vec3(updated_flyby.incoming_soi_state.v, incoming_boundary.v)),
				mag_vec3(subtract_vec3(updated_flyby.outgoing_soi_state.v, outgoing_boundary.v)));
			maximum_velocity_change = fmax(maximum_velocity_change,
				fmax(velocity_change, velocity_mismatch) / velocity_scale);
			double boundary_position_change = fmax(
				mag_vec3(subtract_vec3(updated_flyby.incoming_soi_state.r, flybys[i].incoming_soi_state.r)),
				mag_vec3(subtract_vec3(updated_flyby.outgoing_soi_state.r, flybys[i].outgoing_soi_state.r)));
			double boundary_position_mismatch = fmax(
				mag_vec3(subtract_vec3(updated_flyby.incoming_soi_state.r, incoming_boundary.r)),
				mag_vec3(subtract_vec3(updated_flyby.outgoing_soi_state.r, outgoing_boundary.r)));
			maximum_boundary_position_change = fmax(maximum_boundary_position_change,
			fmax(boundary_position_change, boundary_position_mismatch) / updated_flyby.soi_radius);
			incoming_excess[i] = new_incoming_excess;
			outgoing_excess[i] = new_outgoing_excess;
			flybys[i] = updated_flyby;
		}
		if(!valid_route) break;
		if(maximum_time_change < 1e-3 && maximum_velocity_change < 1e-3 &&
			maximum_boundary_position_change < 2e-3) {
			converged = 1;
			break;
		}
	}
	if(!valid_route || !converged) {
		goto cleanup;
	}

	Body *departure_body = candidate->bodies[0];
	DepartureBurn3D departure_burn;
	Vector3 departure_vinf = subtract_vec3(legs[0].v0, states[0].v);
	if(!calculate_free_plane_departure_burn(departure_vinf, departure_body,
		alt2radius(departure_body, calc_data->dv_filter.dep_periapsis), &departure_burn)) {
		goto cleanup;
	}
	double departure_dv = departure_burn.burn_magnitude;
	if(departure_dv > calc_data->dv_filter.max_depdv) {
		goto cleanup;
	}

	double powered_flyby_dv = 0;
	for(size_t i = 1; i + 1 < candidate->num_steps; i++) {
		powered_flyby_dv += flybys[i].burn_delta_v;
	}

	size_t last_leg = candidate->num_steps - 2;
	Body *arrival_body = candidate->bodies[candidate->num_steps - 1];
	double arrival_dv = arrival_delta_v(&calc_data->dv_filter, arrival_body,
		legs[last_leg].v1, states[candidate->num_steps - 1].v,
		calc_data->dv_filter.arr_periapsis);
	if(arrival_dv > calc_data->dv_filter.max_satdv) {
		goto cleanup;
	}
	total_dv = departure_dv + powered_flyby_dv + arrival_dv;
	if(total_dv > calc_data->dv_filter.max_totdv) {
		total_dv = DBL_MAX;
		goto cleanup;
	}
	for(size_t i = 1; i + 1 < candidate->num_steps; i++) {
		legs[i - 1].v1 = add_vec3(states[i].v, incoming_excess[i]);
		legs[i].v0 = add_vec3(states[i].v, outgoing_excess[i]);
	}

cleanup:
	free(flybys);
	free(incoming_excess);
	free(outgoing_excess);
	return total_dv;
}

static int initialize_candidate(PoweredRouteCandidate *candidate, struct ItinStep *arrival,
		struct ItinStep **departures, int num_departures, double baseline_dv) {
	size_t num_steps = 0;
	for(struct ItinStep *step = arrival; step != NULL; step = step->prev) {
		if(step->body == NULL) return 0;
		num_steps++;
	}
	if(num_steps < 3) return 0;

	candidate->num_steps = num_steps;
	candidate->bodies = malloc(num_steps * sizeof(*candidate->bodies));
	candidate->signature_dates = malloc(num_steps * sizeof(*candidate->signature_dates));
	candidate->signature_positions = malloc(num_steps * sizeof(*candidate->signature_positions));
	candidate->signature_v_arr = malloc(num_steps * sizeof(*candidate->signature_v_arr));
	candidate->signature_v_dep = malloc(num_steps * sizeof(*candidate->signature_v_dep));
	candidate->times = malloc(num_steps * sizeof(*candidate->times));
	candidate->best_states = malloc(num_steps * sizeof(*candidate->best_states));
	candidate->best_legs = malloc((num_steps - 1) * sizeof(*candidate->best_legs));
	if(candidate->bodies == NULL || candidate->signature_dates == NULL ||
		candidate->signature_positions == NULL || candidate->signature_v_arr == NULL ||
		candidate->signature_v_dep == NULL || candidate->times == NULL ||
		candidate->best_states == NULL || candidate->best_legs == NULL) return 0;

	struct ItinStep *nodes[num_steps];
	struct ItinStep *step = arrival;
	for(size_t reverse_index = num_steps; reverse_index > 0; reverse_index--) {
		size_t i = reverse_index - 1;
		nodes[i] = step;
		candidate->bodies[i] = step->body;
		candidate->signature_dates[i] = step->date;
		candidate->signature_positions[i] = step->r;
		candidate->signature_v_arr[i] = step->v_arr;
		candidate->signature_v_dep[i] = step->v_dep;
		candidate->times[i] = step->date;
		step = step->prev;
	}
	struct ItinStep *root = nodes[0];
	candidate->departure_index = -1;
	for(int i = 0; i < num_departures; i++) {
		if(departures[i] == root) {
			candidate->departure_index = i;
			break;
		}
	}
	if(candidate->departure_index < 0) return 0;

	candidate->mutable_start = 1;
	for(size_t i = 0; i + 1 < num_steps; i++)
		if(nodes[i]->num_next_nodes > 1) candidate->mutable_start = i + 1;
	candidate->original_total_dv = baseline_dv;
	candidate->best_total_dv = DBL_MAX;
	return candidate->mutable_start < num_steps;
}

static void free_candidate(PoweredRouteCandidate *candidate) {
	free(candidate->bodies);
	free(candidate->signature_dates);
	free(candidate->signature_positions);
	free(candidate->signature_v_arr);
	free(candidate->signature_v_dep);
	free(candidate->times);
	free(candidate->best_states);
	free(candidate->best_legs);
	memset(candidate, 0, sizeof(*candidate));
}

static int optimize_candidate(PoweredRouteCandidate *candidate, const Itin_Calc_Data *calc_data) {
	OSV *trial_states = malloc(candidate->num_steps * sizeof(*trial_states));
	Lambert3 *trial_legs = malloc((candidate->num_steps - 1) * sizeof(*trial_legs));
	if(trial_states == NULL || trial_legs == NULL) {
		free(trial_states);
		free(trial_legs);
		return 0;
	}
	candidate->best_total_dv = evaluate_route(candidate, calc_data, candidate->times,
		candidate->best_states, candidate->best_legs);
	if(!isfinite(candidate->best_total_dv) || candidate->best_total_dv >= DBL_MAX) {
		free(trial_states);
		free(trial_legs);
		return 0;
	}
	candidate->original_total_dv = candidate->best_total_dv;

	static const double time_steps_days[] = {180, 60, 20, 7, 2, 0.5};
	double latest_arrival = fmin(calc_data->jd_max_arr,
		candidate->times[0] + calc_data->max_duration);
	for(size_t step_index = 0; step_index < sizeof(time_steps_days) / sizeof(time_steps_days[0]); step_index++) {
		for(int sweep = 0; sweep < 2; sweep++) {
			int improved_in_sweep = 0;
			for(size_t i = candidate->mutable_start; i < candidate->num_steps; i++) {
				double lower = candidate->times[i - 1] + POWERED_MIN_LEG_DAYS;
				double upper = latest_arrival;
				if(i + 1 < candidate->num_steps)
					upper = fmin(upper, candidate->times[i + 1] - POWERED_MIN_LEG_DAYS);
				for(int direction = -1; direction <= 1; direction += 2) {
					double old_time = candidate->times[i];
					double trial_time = old_time + direction * time_steps_days[step_index];
					if(trial_time < lower || trial_time > upper) continue;
					candidate->times[i] = trial_time;
					double trial_dv = evaluate_route(candidate, calc_data, candidate->times,
						trial_states, trial_legs);
					if(trial_dv + POWERED_IMPROVEMENT_THRESHOLD < candidate->best_total_dv) {
						candidate->best_total_dv = trial_dv;
						memcpy(candidate->best_states, trial_states,
							candidate->num_steps * sizeof(*trial_states));
						memcpy(candidate->best_legs, trial_legs,
							(candidate->num_steps - 1) * sizeof(*trial_legs));
						improved_in_sweep = 1;
					} else {
						candidate->times[i] = old_time;
					}
				}
			}
			if(!improved_in_sweep) break;
		}
	}
	free(trial_states);
	free(trial_legs);
	return 1;
}

static int vector_equal(Vector3 first, Vector3 second) {
	return mag_vec3(subtract_vec3(first, second)) < 1e-6;
}

static int route_node_matches(const PoweredRouteCandidate *candidate,
		const struct ItinStep *step, size_t index) {
	return step != NULL && step->body == candidate->bodies[index] &&
		fabs(step->date - candidate->signature_dates[index]) < 1e-9 &&
		vector_equal(step->r, candidate->signature_positions[index]) &&
		vector_equal(step->v_arr, candidate->signature_v_arr[index]) &&
		vector_equal(step->v_dep, candidate->signature_v_dep[index]);
}

static struct ItinStep *find_matching_leaf(struct ItinStep *step,
		const PoweredRouteCandidate *candidate, size_t index) {
	if(index >= candidate->num_steps || !route_node_matches(candidate, step, index)) return NULL;
	if(index + 1 == candidate->num_steps) return step->num_next_nodes == 0 ? step : NULL;
	for(int child = 0; child < step->num_next_nodes; child++) {
		struct ItinStep *match = find_matching_leaf(step->next[child], candidate, index + 1);
		if(match != NULL) return match;
	}
	return NULL;
}

static struct ItinStep *find_matching_prefix(struct ItinStep *step,
		const PoweredRouteCandidate *candidate, size_t prefix_length, size_t index) {
	if(index >= prefix_length || !route_node_matches(candidate, step, index)) return NULL;
	if(index + 1 == prefix_length) return step;
	for(int child = 0; child < step->num_next_nodes; child++) {
		struct ItinStep *match = find_matching_prefix(step->next[child], candidate, prefix_length, index + 1);
		if(match != NULL) return match;
	}
	return NULL;
}

static struct ItinStep *make_optimized_suffix(const PoweredRouteCandidate *candidate) {
	size_t suffix_count = candidate->num_steps - candidate->mutable_start;
	struct ItinStep **nodes = calloc(suffix_count, sizeof(*nodes));
	if(nodes == NULL) return NULL;
	for(size_t i = 0; i < suffix_count; i++) {
		nodes[i] = calloc(1, sizeof(*nodes[i]));
		if(nodes[i] == NULL) {
			for(size_t j = 0; j < i; j++) free(nodes[j]);
			free(nodes);
			return NULL;
		}
	}
	for(size_t i = 0; i < suffix_count; i++) {
		size_t route_index = candidate->mutable_start + i;
		struct ItinStep *node = nodes[i];
		node->body = candidate->bodies[route_index];
		node->date = candidate->times[route_index];
		node->r = candidate->best_states[route_index].r;
		node->v_body = candidate->best_states[route_index].v;
		node->v_arr = candidate->best_legs[route_index - 1].v1;
		if(route_index + 1 < candidate->num_steps) {
			node->next = malloc(sizeof(*node->next));
			if(node->next == NULL) {
				for(size_t j = 0; j < suffix_count; j++) {
					free(nodes[j]->next);
					free(nodes[j]);
				}
				free(nodes);
				return NULL;
			}
			node->num_next_nodes = 1;
			node->v_dep = candidate->best_legs[route_index - 1].v0;
			node->next[0] = nodes[i + 1];
		}
	}
	for(size_t i = 0; i < suffix_count; i++)
		nodes[i]->prev = i == 0 ? NULL : nodes[i - 1];
	struct ItinStep *first = nodes[0];
	free(nodes);
	return first;
}

static int replace_candidate_route(struct ItinStep **departures, PoweredRouteCandidate *candidate) {
	struct ItinStep *original_root = departures[candidate->departure_index];
	struct ItinStep *root = create_itin_copy(original_root);
	if(root == NULL) return 0;
	struct ItinStep *leaf = find_matching_leaf(root, candidate, 0);
	if(leaf == NULL) {
		free_itinerary(root);
		return 0;
	}
	struct ItinStep *old_suffix = leaf;
	for(size_t i = candidate->num_steps - 1; i > candidate->mutable_start; i--)
		old_suffix = old_suffix->prev;
	struct ItinStep *parent = old_suffix->prev;
	if(parent == NULL) {
		free_itinerary(root);
		return 0;
	}
	int child_index = -1;
	for(int i = 0; i < parent->num_next_nodes; i++) {
		if(parent->next[i] == old_suffix) {
			child_index = i;
			break;
		}
	}
	if(child_index < 0) {
		free_itinerary(root);
		return 0;
	}
	struct ItinStep *suffix = make_optimized_suffix(candidate);
	if(suffix == NULL) {
		free_itinerary(root);
		return 0;
	}
	struct ItinStep **expanded_children = realloc(parent->next,
		(parent->num_next_nodes + 1) * sizeof(*parent->next));
	if(expanded_children == NULL) {
		free_itinerary(suffix);
		free_itinerary(root);
		return 0;
	}
	parent->next = expanded_children;
	for(int i = child_index; i + 1 < parent->num_next_nodes; i++)
		parent->next[i] = parent->next[i + 1];
	parent->num_next_nodes--;
	free_itinerary(old_suffix);
	suffix->prev = parent;
	parent->next[parent->num_next_nodes++] = suffix;
	departures[candidate->departure_index] = root;
	free_itinerary(original_root);
	return 1;
}

static void insert_top_route(int route_index, double cost, int max_routes, int *count,
		int indices[POWERED_ROUTE_LIMIT], double costs[POWERED_ROUTE_LIMIT]) {
	if(*count == max_routes && cost >= costs[*count - 1]) return;
	int insert = *count < max_routes ? *count : max_routes - 1;
	while(insert > 0 && cost < costs[insert - 1]) {
		if(insert < max_routes) {
			costs[insert] = costs[insert - 1];
			indices[insert] = indices[insert - 1];
		}
		insert--;
	}
	costs[insert] = cost;
	indices[insert] = route_index;
	if(*count < max_routes) (*count)++;
}

int optimize_powered_flyby_routes(struct ItinStep **departures, int num_departures,
		const Itin_Calc_Data *calc_data, int max_routes, PoweredFlybyOptimizationStats *stats) {
	if(stats != NULL) *stats = (PoweredFlybyOptimizationStats){0};
	if(departures == NULL || num_departures <= 0 || calc_data == NULL || max_routes <= 0 ||
		calc_data->seq_info.to_target.type != ITIN_SEQ_INFO_TO_TARGET) return 0;
	int num_itineraries = 0;
	for(int i = 0; i < num_departures; i++) num_itineraries += get_number_of_itineraries(departures[i]);
	if(num_itineraries <= 0) return 0;
	struct PorkchopPoint *points = create_porkchop_array_from_departures(departures, num_departures,
		calc_data->dv_filter.dep_periapsis, calc_data->dv_filter.arr_periapsis);
	if(points == NULL) return 0;

	int top_indices[POWERED_ROUTE_LIMIT];
	double top_costs[POWERED_ROUTE_LIMIT];
	int top_count = 0;
	int route_limit = max_routes < POWERED_ROUTE_LIMIT ? max_routes : POWERED_ROUTE_LIMIT;
	for(int point_index = 0; point_index < num_itineraries; point_index++) {
		if(get_num_of_itin_layers(get_first(points[point_index].arrival)) < 3) continue;
		double arrival_dv = arrival_delta_v(&calc_data->dv_filter,
			points[point_index].arrival->body, points[point_index].arrival->v_arr,
			points[point_index].arrival->v_body, calc_data->dv_filter.arr_periapsis);
		double total_dv = points[point_index].dv_dep + points[point_index].dv_dsm +
			points[point_index].dv_flyby + arrival_dv;
		if(isfinite(total_dv))
			insert_top_route(point_index, total_dv, route_limit, &top_count, top_indices, top_costs);
	}
	PoweredRouteCandidate *candidates = calloc((size_t)top_count, sizeof(*candidates));
	if(candidates == NULL) {
		free(points);
		return 0;
	}
	int candidates_optimized = 0;
	for(int candidate_index = 0; candidate_index < top_count; candidate_index++) {
		PoweredRouteCandidate *candidate = &candidates[candidates_optimized];
		if(!initialize_candidate(candidate, points[top_indices[candidate_index]].arrival,
			departures, num_departures, top_costs[candidate_index])) {
			free_candidate(candidate);
			continue;
		}
		if(optimize_candidate(candidate, calc_data)) {
			candidates_optimized++;
			if(stats != NULL) stats->routes_valid++;
		} else free_candidate(candidate);
	}
	free(points);

	int improvements = 0;
	for(int i = 0; i < candidates_optimized; i++) {
		PoweredRouteCandidate *candidate = &candidates[i];
		if(replace_candidate_route(departures, candidate)) {
			if(stats != NULL) stats->routes_corrected++;
			double saving = candidate->original_total_dv - candidate->best_total_dv;
			if(saving > POWERED_IMPROVEMENT_THRESHOLD) {
				improvements++;
				if(stats != NULL && saving > stats->best_delta_v_saving)
					stats->best_delta_v_saving = saving;
			}
		}
		free_candidate(candidate);
	}
	free(candidates);
	if(stats != NULL) {
		stats->routes_considered = top_count;
		stats->routes_improved = improvements;
	}
	return improvements;
}
