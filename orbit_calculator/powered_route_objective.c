#include "powered_route.h"
#include "leg_variants.h"
#include "flyby_policy.h"
#include "search_cancel.h"
#include "soi_encounter.h"
#include "tools/starting_orbit.h"
#include <stdlib.h>
#include <string.h>

/* Residual is in units of the convergence tolerance. */
#define POWERED_STALL_CHECK_ITERATION 25
#define POWERED_FREEZE_ITERATION 4
#define POWERED_STALL_RESIDUAL 30.0

_Thread_local int objective_budget = -1;
_Thread_local unsigned long long objective_calls;

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

/* One leg: a Lambert arc, or with a DSM a ballistic coast from the shifted departure velocity
 * followed by a second Lambert arc. A zero delta_v reproduces the plain arc with zero burn. */
static int solve_leg(const PoweredRouteCandidate *candidate, size_t index, CelestSystem *system,
		Vector3 departure_position, Vector3 arrival_position, double departure_epoch,
		double duration_days, int variant, Lambert3 *leg, PoweredDsmResult *dsm) {
	Lambert3 plain = calc_leg_variant_lambert(system->cb, departure_position, arrival_position,
		duration_days * 86400, variant, itinerary_search_cancelled, NULL);
	if(itinerary_search_cancelled(NULL) || !is_usable_lambert(plain)) return 0;
	*leg = plain;
	if(dsm != NULL) dsm->used = 0;
	const PoweredLegDsm *spec = candidate->dsms != NULL ? &candidate->dsms[index] : NULL;
	if(spec == NULL || !spec->enabled) return 1;
	// A leg back to the same body spans revolutions, so each of its two arcs may take any revolution variant.
	int resonant = candidate->bodies[index] == candidate->bodies[index + 1];
	int arc_variants = resonant ? LEG_VARIANT_COUNT : 1;
	if(spec->use_position) {
		// The leg's epochs are SOI-adjusted, so the maneuver is given by absolute date.
		double eta = (spec->date - departure_epoch) / duration_days;
		if(!(eta > 0 && eta < 1) || !is_finite_vector(spec->position)) return 0;
		Lambert3 firsts[LEG_VARIANT_COUNT], seconds[LEG_VARIANT_COUNT];
		int first_valid[LEG_VARIANT_COUNT], second_valid[LEG_VARIANT_COUNT];
		for(int v = 0; v < arc_variants; v++) {
			firsts[v] = calc_leg_variant_lambert(system->cb, departure_position, spec->position,
				eta * duration_days * 86400, v, itinerary_search_cancelled, NULL);
			seconds[v] = calc_leg_variant_lambert(system->cb, spec->position, arrival_position,
				(1 - eta) * duration_days * 86400, v, itinerary_search_cancelled, NULL);
			first_valid[v] = is_usable_lambert(firsts[v]);
			second_valid[v] = is_usable_lambert(seconds[v]);
		}
		if(itinerary_search_cancelled(NULL)) return 0;
		double least_burn = DBL_MAX;
		Lambert3 first = {0}, second = {0};
		for(int a = 0; a < arc_variants; a++) {
			for(int b = 0; b < arc_variants && first_valid[a]; b++) {
				if(!second_valid[b]) continue;
				double burn = mag_vec3(subtract_vec3(seconds[b].v0, firsts[a].v1));
				if(burn < least_burn) { least_burn = burn; first = firsts[a]; second = seconds[b]; }
			}
		}
		if(!(least_burn < DBL_MAX)) return 0;
		leg->v0 = first.v0;
		leg->v1 = second.v1;
		if(dsm != NULL) {
			dsm->used = 1;
			dsm->date = spec->use_position ? spec->date : departure_epoch + spec->eta * duration_days;
			dsm->position = spec->position;
			dsm->velocity_before = first.v1;
			dsm->velocity_after = second.v0;
			dsm->delta_v = mag_vec3(subtract_vec3(second.v0, first.v1));
		}
		return 1;
	}
	if(!(spec->eta > 0 && spec->eta < 1) || !is_finite_vector(spec->delta_v)) return 0;

	Vector3 departure_velocity = add_vec3(plain.v0, spec->delta_v);
	OSV at_dsm = propagate_osv_time((OSV){departure_position, departure_velocity}, system->cb,
		spec->eta * duration_days * 86400);
	if(!is_finite_vector(at_dsm.r) || !is_finite_vector(at_dsm.v)) return 0;
	Lambert3 second;
	double least_burn = DBL_MAX;
	for(int v = 0; v < arc_variants; v++) {
		Lambert3 trial = calc_leg_variant_lambert(system->cb, at_dsm.r, arrival_position,
			(1 - spec->eta) * duration_days * 86400, v, itinerary_search_cancelled, NULL);
		if(itinerary_search_cancelled(NULL)) return 0;
		if(!is_usable_lambert(trial)) continue;
		double burn = mag_vec3(subtract_vec3(trial.v0, at_dsm.v));
		if(burn < least_burn) { least_burn = burn; second = trial; }
	}
	if(!(least_burn < DBL_MAX)) return 0;

	leg->v0 = departure_velocity;
	leg->v1 = second.v1;
	if(dsm != NULL) {
		dsm->used = 1;
		dsm->date = departure_epoch + spec->eta * duration_days;
		dsm->position = at_dsm.r;
		dsm->velocity_before = at_dsm.v;
		dsm->velocity_after = second.v0;
		dsm->delta_v = mag_vec3(subtract_vec3(second.v0, at_dsm.v));
	}
	return 1;
}

// A flyby whose moon-free optimum hits a moon is flown at a fixed periapsis radius and side instead.
static int route_flyby(Vector3 incoming, Vector3 outgoing, Body *body, double minimum_periapsis,
		double forced_radius, int forced_side, PoweredFlybyResult *result) {
	if(forced_radius > 0)
		return calculate_powered_flyby_at_periapsis(incoming, outgoing, vec3(0, 0, 0), body, forced_radius, forced_side, result);
	return calculate_powered_flyby(incoming, outgoing, vec3(0, 0, 0), body, minimum_periapsis, result);
}

static int route_crosses_unmodeled_soi(const PoweredRouteCandidate *candidate, CelestSystem *system,
		const double *times, const PoweredFlybyResult *flybys, const Lambert3 *legs,
		const PoweredDsmResult *dsms) {
	for(size_t i = 0; i + 1 < candidate->num_steps; i++) {
		double leg_start = times[i];
		double leg_end = times[i + 1];
		if(i > 0) leg_start += flybys[i].outgoing_soi_time / 86400;
		if(i + 1 < candidate->num_steps - 1)
			leg_end -= flybys[i + 1].incoming_soi_time / 86400;
		OSV coast_start = {legs[i].r0, legs[i].v0};
		if(i > 0) {
			OSV body_at_exit;
			if(!body_state_at(system, candidate->bodies[i], leg_start, &body_at_exit)) return 1;
			coast_start.r = add_vec3(body_at_exit.r, flybys[i].outgoing_soi_state.r);
			coast_start.v = add_vec3(body_at_exit.v, flybys[i].outgoing_soi_state.v);
		}
		if(dsms[i].used) {
			if(coast_leg_crosses_unmodeled_soi(system, candidate->bodies[i], candidate->bodies[i + 1],
				coast_start, leg_start, dsms[i].date) ||
				coast_leg_crosses_unmodeled_soi(system, candidate->bodies[i], candidate->bodies[i + 1],
					(OSV){dsms[i].position, dsms[i].velocity_after}, dsms[i].date, leg_end)) return 1;
		} else if(coast_leg_crosses_unmodeled_soi(system, candidate->bodies[i], candidate->bodies[i + 1],
			coast_start, leg_start, leg_end)) {
			return 1;
		}
	}
	return 0;
}

static double evaluate_route_variants_core(const PoweredRouteCandidate *candidate, const Itin_Calc_Data *calc_data,
		const double *times, OSV *states, Lambert3 *legs, PoweredDsmResult *dsm_out, const int *variants) {
	if(itinerary_search_cancelled(NULL)) return DBL_MAX;
	if(objective_budget == 0) return DBL_MAX;
	if(objective_budget > 0) objective_budget--;
	objective_calls++;
	CelestSystem *system = calc_data->seq_info.to_target.system;
	double latest_arrival = fmin(calc_data->jd_max_arr, times[0] + calc_data->max_duration);
	if(times[candidate->num_steps - 1] > latest_arrival) return DBL_MAX;

	for(size_t i = 0; i < candidate->num_steps && !itinerary_search_cancelled(NULL); i++) {
		Body *body = candidate->bodies[i];
		if(!body_state_at(system, body, times[i], &states[i])) return DBL_MAX;
	}
	if(itinerary_search_cancelled(NULL)) return DBL_MAX;

	for(size_t i = 0; i + 1 < candidate->num_steps && !itinerary_search_cancelled(NULL); i++) {
		double duration_days = times[i + 1] - times[i];
		if(duration_days < powered_min_leg_days(calc_data)) return DBL_MAX;
		if(!solve_leg(candidate, i, system, states[i].r, states[i + 1].r, times[i],
			duration_days, variants != NULL ? variants[i] : 0, &legs[i], NULL)) return DBL_MAX;
	}

	PoweredFlybyResult *flybys = calloc(candidate->num_steps, sizeof(*flybys));
	Vector3 *incoming_excess = calloc(candidate->num_steps, sizeof(*incoming_excess));
	Vector3 *outgoing_excess = calloc(candidate->num_steps, sizeof(*outgoing_excess));
	PoweredDsmResult *dsm_local = calloc(candidate->num_steps, sizeof(*dsm_local));
	double *forced_radius = calloc(candidate->num_steps, sizeof(*forced_radius));
	int *forced_side = calloc(candidate->num_steps, sizeof(*forced_side));
	double *frozen_radius = calloc(candidate->num_steps, sizeof(*frozen_radius));
	int *frozen_side = calloc(candidate->num_steps, sizeof(*frozen_side));
	if(flybys == NULL || incoming_excess == NULL || outgoing_excess == NULL || dsm_local == NULL ||
		forced_radius == NULL || forced_side == NULL || frozen_radius == NULL || frozen_side == NULL) {
		free(frozen_radius);
		free(frozen_side);
		free(flybys);
		free(incoming_excess);
		free(outgoing_excess);
		free(dsm_local);
		free(forced_radius);
		free(forced_side);
		return DBL_MAX;
	}
	double total_dv = DBL_MAX;
	int valid_route = 1;
	for(size_t i = 1; i + 1 < candidate->num_steps && !itinerary_search_cancelled(NULL); i++) {
		Body *body = candidate->bodies[i];
		double minimum_periapsis = flyby_minimum_periapsis(body);
		incoming_excess[i] = candidate->seed_incoming != NULL ? candidate->seed_incoming[i] : subtract_vec3(legs[i - 1].v1, states[i].v);
		outgoing_excess[i] = candidate->seed_outgoing != NULL ? candidate->seed_outgoing[i] : subtract_vec3(legs[i].v0, states[i].v);
		if(!route_flyby(incoming_excess[i], outgoing_excess[i], body, minimum_periapsis,
			forced_radius[i], forced_side[i], &flybys[i])) {
			valid_route = 0;
			break;
		}
	}
	if(itinerary_search_cancelled(NULL)) valid_route = 0;

	int replanned = 0;
iterate:;
	int converged = 0;
	memset(frozen_radius, 0, candidate->num_steps * sizeof(*frozen_radius));
	double best_residual = DBL_MAX;
	for(int iteration = 0; valid_route && !itinerary_search_cancelled(NULL) && iteration < 60; iteration++) {
		for(size_t i = 0; i + 1 < candidate->num_steps && !itinerary_search_cancelled(NULL); i++) {
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
			if(duration_days < powered_min_leg_days(calc_data)) {
				valid_route = 0;
				break;
			}
			if(!solve_leg(candidate, i, system, departure_position, arrival_position,
				departure_epoch, duration_days, variants != NULL ? variants[i] : 0, &legs[i], &dsm_local[i])) {
				valid_route = 0;
				break;
			}
		}
		if(!valid_route || itinerary_search_cancelled(NULL)) break;

		double maximum_time_change = 0;
		double maximum_velocity_change = 0;
		double maximum_boundary_position_change = 0;
		for(size_t i = 1; i + 1 < candidate->num_steps && !itinerary_search_cancelled(NULL); i++) {
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
			double minimum_periapsis = flyby_minimum_periapsis(body);
			double use_radius = forced_radius[i] > 0 ? forced_radius[i] : frozen_radius[i];
			int use_side = forced_radius[i] > 0 ? forced_side[i] : frozen_side[i];
			// The frozen choice is only a damping aid, so fall back to the free optimum if it stops being feasible.
			if(!route_flyby(new_incoming_excess, new_outgoing_excess, body, minimum_periapsis,
				use_radius, use_side, &updated_flyby) &&
				!(use_radius > 0 && forced_radius[i] <= 0 && route_flyby(new_incoming_excess, new_outgoing_excess,
					body, minimum_periapsis, 0, 0, &updated_flyby))) {
				valid_route = 0;
				break;
			}
			if(iteration + 1 == POWERED_FREEZE_ITERATION && frozen_radius[i] <= 0) {
				// The periapsis radius and side flip between near-equal optima, which keeps the iteration from settling.
				Vector3 plane_normal = cross_vec3(new_incoming_excess, new_outgoing_excess);
				frozen_radius[i] = updated_flyby.periapsis_radius;
				frozen_side[i] = dot_vec3(updated_flyby.periapsis_direction, plane_normal) >= 0 ? 1 : -1;
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
		if(!valid_route || itinerary_search_cancelled(NULL)) break;
		if(maximum_time_change < 1e-3 && maximum_velocity_change < 1e-3 &&
			maximum_boundary_position_change < 2e-3) {
			converged = 1;
			break;
		}
		// Oscillating routes never get near the tolerance, so they are not worth the full iteration budget.
		best_residual = fmin(best_residual, fmax(fmax(maximum_time_change / 1e-3, maximum_velocity_change / 1e-3),
			maximum_boundary_position_change / 2e-3));
		if(iteration >= POWERED_STALL_CHECK_ITERATION && best_residual > POWERED_STALL_RESIDUAL) break;
	}
	if(candidate->converged_out != NULL) *candidate->converged_out = converged;
	if(!valid_route || (!converged && !candidate->accept_unconverged) || itinerary_search_cancelled(NULL)) {
		goto cleanup;
	}

	// Checked once on the converged flybys; the iteration above changes them too often to check each time.
	int blocked_flybys = 0;
	for(size_t i = 1; i + 1 < candidate->num_steps; i++) {
		if(!flyby_blocked_by_moon(candidate->bodies[i], &flybys[i], times[i])) continue;
		// A second pass that is blocked again is not worth chasing.
		if(replanned || !plan_flyby_around_moons(incoming_excess[i], outgoing_excess[i], vec3(0, 0, 0), candidate->bodies[i],
			flyby_minimum_periapsis(candidate->bodies[i]), times[i], NULL, &forced_radius[i], &forced_side[i])) goto cleanup;
		blocked_flybys++;
	}
	if(blocked_flybys > 0) {
		for(size_t i = 1; i + 1 < candidate->num_steps; i++)
			if(forced_radius[i] > 0 && !route_flyby(incoming_excess[i], outgoing_excess[i], candidate->bodies[i],
				flyby_minimum_periapsis(candidate->bodies[i]), forced_radius[i], forced_side[i], &flybys[i])) goto cleanup;
		replanned = 1;
		goto iterate;
	}

	Body *departure_body = candidate->bodies[0];
	Vector3 departure_vinf = subtract_vec3(legs[0].v0, states[0].v);
	double departure_dv;
	if(!starting_orbit_departure_dv(departure_body, departure_vinf,
		alt2radius(departure_body, calc_data->dv_filter.dep_periapsis), &departure_dv)) {
		goto cleanup;
	}
	if(departure_dv > calc_data->dv_filter.max_depdv) {
		goto cleanup;
	}

	double powered_flyby_dv = 0;
	for(size_t i = 1; i + 1 < candidate->num_steps; i++) {
		powered_flyby_dv += flybys[i].burn_delta_v;
	}

	double dsm_total_dv = 0;
	for(size_t i = 0; i + 1 < candidate->num_steps; i++)
		if(dsm_local[i].used) dsm_total_dv += dsm_local[i].delta_v;

	size_t last_leg = candidate->num_steps - 2;
	Body *arrival_body = candidate->bodies[candidate->num_steps - 1];
	double arrival_dv = arrival_delta_v(&calc_data->dv_filter, arrival_body,
		legs[last_leg].v1, states[candidate->num_steps - 1].v,
		calc_data->dv_filter.arr_periapsis);
	if(calc_data->dv_filter.last_transfer_type == TF_CAPTURE || calc_data->dv_filter.last_transfer_type == TF_CIRC)
		arrival_dv = arrival_dv_with_assist(arrival_body, subtract_vec3(legs[last_leg].v1, states[candidate->num_steps - 1].v),
			alt2radius(arrival_body, calc_data->dv_filter.arr_periapsis), calc_data->dv_filter.last_transfer_type == TF_CIRC,
			arrival_dv, times[candidate->num_steps - 1], NULL);
	if(arrival_dv + dsm_total_dv + powered_flyby_dv > calc_data->dv_filter.max_satdv) {
		goto cleanup;
	}
	total_dv = departure_dv + powered_flyby_dv + dsm_total_dv + arrival_dv;
	if(total_dv > calc_data->dv_filter.max_totdv) {
		total_dv = DBL_MAX;
		goto cleanup;
	}
	for(size_t i = 1; i + 1 < candidate->num_steps; i++) {
		legs[i - 1].v1 = add_vec3(states[i].v, incoming_excess[i]);
		legs[i].v0 = add_vec3(states[i].v, outgoing_excess[i]);
	}
	if(candidate->check_unmodeled_soi &&
		route_crosses_unmodeled_soi(candidate, system, times, flybys, legs, dsm_local)) {
		if(candidate->unmodeled_soi_crossing_out != NULL)
			*candidate->unmodeled_soi_crossing_out = 1;
		total_dv = DBL_MAX;
		goto cleanup;
	}
	if(dsm_out != NULL)
		memcpy(dsm_out, dsm_local, (candidate->num_steps - 1) * sizeof(*dsm_out));

cleanup:
	free(frozen_radius);
	free(frozen_side);
	free(flybys);
	free(incoming_excess);
	free(outgoing_excess);
	free(dsm_local);
	free(forced_radius);
	free(forced_side);
	return total_dv;
}

double evaluate_route_core(const PoweredRouteCandidate *candidate, const Itin_Calc_Data *calc_data,
		const double *times, OSV *states, Lambert3 *legs, PoweredDsmResult *dsm_out) {
	if(candidate->unmodeled_soi_crossing_out != NULL)
		*candidate->unmodeled_soi_crossing_out = 0;
	if(candidate->leg_variants != NULL)
		return evaluate_route_variants_core(candidate, calc_data, times, states, legs, dsm_out, candidate->leg_variants);
	size_t resonant[2];
	int num_resonant = 0;
	for(size_t i = 0; i + 1 < candidate->num_steps; i++) {
		if(candidate->bodies[i] != candidate->bodies[i + 1]) continue;
		if(num_resonant == 2) return DBL_MAX;
		resonant[num_resonant++] = i;
	}
	if(num_resonant == 0)
		return evaluate_route_variants_core(candidate, calc_data, times, states, legs, dsm_out, NULL);

	// Legs between two visits of the same body can span revolutions, so keep the best leg variant.
	size_t num_legs = candidate->num_steps - 1;
	int *variants = calloc(num_legs, sizeof(*variants));
	OSV *trial_states = calloc(candidate->num_steps, sizeof(*trial_states));
	Lambert3 *trial_legs = calloc(num_legs, sizeof(*trial_legs));
	PoweredDsmResult *trial_dsm = calloc(num_legs, sizeof(*trial_dsm));
	double best = DBL_MAX;
	int best_converged = 0;
	int radix[2];
	int combinations = 1;
	int any_soi_crossing = 0;
	for(int r = 0; r < num_resonant; r++) {
		// A leg with a fixed-position maneuver picks its arc variants itself.
		int fixed_maneuver = candidate->dsms != NULL && candidate->dsms[resonant[r]].enabled &&
			candidate->dsms[resonant[r]].use_position;
		radix[r] = fixed_maneuver ? 1 : LEG_VARIANT_COUNT;
		combinations *= radix[r];
	}
	for(int combination = 0; combination < combinations && variants != NULL && trial_states != NULL &&
		trial_legs != NULL && trial_dsm != NULL && !itinerary_search_cancelled(NULL); combination++) {
		int remainder = combination;
		for(int r = 0; r < num_resonant; r++) {
			variants[resonant[r]] = remainder % radix[r];
			remainder /= radix[r];
		}
		PoweredRouteCandidate trial_candidate = *candidate;
		int trial_soi_crossing = 0;
		trial_candidate.unmodeled_soi_crossing_out = candidate->check_unmodeled_soi ?
			&trial_soi_crossing : NULL;
		int trial_converged = 0;
		trial_candidate.converged_out = &trial_converged;
		double dv = evaluate_route_variants_core(&trial_candidate, calc_data, times, trial_states,
			trial_legs, trial_dsm, variants);
		if(!(dv < DBL_MAX)) {
			any_soi_crossing |= trial_soi_crossing;
			continue;
		}
		int better = best >= DBL_MAX || (trial_converged && !best_converged) ||
			(trial_converged == best_converged && dv < best);
		if(!better) continue;
		best = dv;
		best_converged = trial_converged;
		memcpy(states, trial_states, candidate->num_steps * sizeof(*states));
		memcpy(legs, trial_legs, num_legs * sizeof(*legs));
		if(dsm_out != NULL) memcpy(dsm_out, trial_dsm, num_legs * sizeof(*dsm_out));
	}
	if(candidate->converged_out != NULL) *candidate->converged_out = best_converged;
	if(candidate->unmodeled_soi_crossing_out != NULL)
		*candidate->unmodeled_soi_crossing_out = best < DBL_MAX ? 0 : any_soi_crossing;
	free(variants);
	free(trial_states);
	free(trial_legs);
	free(trial_dsm);
	return best;
}

double evaluate_route(const PoweredRouteCandidate *candidate, const Itin_Calc_Data *calc_data,
		const double *times, OSV *states, Lambert3 *legs) {
	return evaluate_route_core(candidate, calc_data, times, states, legs, NULL);
}

double evaluate_powered_flyby_route_objective_with_dsm(const Itin_Calc_Data *calc_data,
		Body **bodies, const double *times, size_t num_steps, const PoweredLegDsm *dsms) {
	if(calc_data == NULL || bodies == NULL || times == NULL || num_steps < 3) return DBL_MAX;
	for(size_t i = 0; i < num_steps; i++)
		if(bodies[i] == NULL || !isfinite(times[i])) return DBL_MAX;
	OSV *states = calloc(num_steps, sizeof(*states));
	Lambert3 *legs = calloc(num_steps - 1, sizeof(*legs));
	if(states == NULL || legs == NULL) {
		free(states);
		free(legs);
		return DBL_MAX;
	}
	PoweredRouteCandidate candidate = {.num_steps = num_steps, .bodies = bodies,
		.dsms = (PoweredLegDsm *)dsms};
	double objective = evaluate_route(&candidate, calc_data, times, states, legs);
	free(states);
	free(legs);
	return objective;
}

double evaluate_powered_flyby_route_objective(const Itin_Calc_Data *calc_data,
		Body **bodies, const double *times, size_t num_steps) {
	return evaluate_powered_flyby_route_objective_with_dsm(calc_data, bodies, times, num_steps, NULL);
}

int powered_route_has_unmodeled_soi_crossing(const Itin_Calc_Data *source, struct ItinStep *arrival,
		int *crosses) {
	if(crosses != NULL) *crosses = 0;
	if(source == NULL || arrival == NULL || arrival->body == NULL || arrival->num_next_nodes != 0 ||
		source->seq_info.to_target.type != ITIN_SEQ_INFO_TO_TARGET ||
		source->seq_info.to_target.system == NULL) return 0;
	struct ItinStep *first = get_first(arrival);
	if(first == NULL || first->body == NULL) return 0;

	size_t num_nodes = 0, num_steps = 0;
	for(struct ItinStep *node = first; node != NULL; node = node->next != NULL ? node->next[0] : NULL) {
		if(node->num_next_nodes > 1) return 0;
		num_nodes++;
		num_steps += node->body != NULL;
	}
	if(num_steps < 2) return 0;

	Body **bodies = calloc(num_steps, sizeof(*bodies));
	double *times = calloc(num_steps, sizeof(*times));
	PoweredLegDsm *dsms = calloc(num_steps - 1, sizeof(*dsms));
	Vector3 *seed_incoming = calloc(num_steps, sizeof(*seed_incoming));
	Vector3 *seed_outgoing = calloc(num_steps, sizeof(*seed_outgoing));
	OSV *states = calloc(num_steps, sizeof(*states));
	Lambert3 *legs = calloc(num_steps - 1, sizeof(*legs));
	if(bodies == NULL || times == NULL || dsms == NULL || seed_incoming == NULL ||
		seed_outgoing == NULL || states == NULL || legs == NULL) {
		free(bodies); free(times); free(dsms); free(seed_incoming); free(seed_outgoing);
		free(states); free(legs);
		return 0;
	}

	size_t index = 0;
	int valid = 1;
	for(struct ItinStep *node = first; node != NULL && valid;
		node = node->next != NULL ? node->next[0] : NULL) {
		if(node->body == NULL) continue;
		bodies[index] = node->body;
		times[index++] = node->date;
	}
	for(size_t i = 0; i + 1 < num_steps; i++)
		if(!(times[i + 1] > times[i])) valid = 0;

	index = 0;
	for(struct ItinStep *node = first; node != NULL && valid;
		node = node->next != NULL ? node->next[0] : NULL) {
		if(node->body != NULL) {
			if(index > 0 && index + 1 < num_steps) {
				OSV body_state;
				if(node->next == NULL || !body_state_at(source->seq_info.to_target.system,
					node->body, node->date, &body_state)) {
					valid = 0;
					break;
				}
				seed_incoming[index] = subtract_vec3(node->v_arr, body_state.v);
				seed_outgoing[index] = subtract_vec3(node->next[0]->v_dep, body_state.v);
				if(!is_finite_vector(seed_incoming[index]) || !is_finite_vector(seed_outgoing[index]) ||
					!(mag_vec3(seed_incoming[index]) > 0) || !(mag_vec3(seed_outgoing[index]) > 0))
					valid = 0;
			}
			index++;
			continue;
		}
		if(index == 0 || index >= num_steps || dsms[index - 1].enabled) {
			valid = 0;
			break;
		}
		dsms[index - 1] = (PoweredLegDsm){.enabled = 1, .use_position = 1,
			.position = node->r, .date = node->date};
	}

	double objective = DBL_MAX;
	int crossing = 0;
	if(valid) {
		Itin_Calc_Data calc_data = *source;
		calc_data.jd_max_arr = times[num_steps - 1] + 1;
		calc_data.max_duration = times[num_steps - 1] - times[0] + 1;
		calc_data.dv_filter.max_totdv = 1e12;
		calc_data.dv_filter.max_depdv = 1e12;
		calc_data.dv_filter.max_satdv = 1e12;
		PoweredRouteCandidate candidate = {.num_steps = num_steps, .bodies = bodies, .times = times,
			.dsms = dsms, .seed_incoming = seed_incoming, .seed_outgoing = seed_outgoing,
			.accept_unconverged = 1, .check_unmodeled_soi = 1,
			.unmodeled_soi_crossing_out = &crossing};
		int saved_budget = objective_budget;
		objective_budget = -1;
		objective = evaluate_route_core(&candidate, &calc_data, times, states, legs, NULL);
		objective_budget = saved_budget;
	}
	if(crosses != NULL) *crosses = crossing;
	free(bodies); free(times); free(dsms); free(seed_incoming); free(seed_outgoing);
	free(states); free(legs);
	(void)num_nodes;
	return valid && (objective < DBL_MAX || crossing);
}

static int resolve_itinerary_internal(CelestSystem *system, struct ItinStep *first,
		double dep_periapsis, double arr_periapsis, enum LastTransferType last_transfer_type,
		int accept_unconverged, int *converged_out) {
	if(system == NULL || first == NULL || first->prev != NULL || first->body == NULL) return 0;
	size_t num_nodes = 0, num_steps = 0;
	struct ItinStep *last = first;
	for(struct ItinStep *node = first; node != NULL; node = node->next != NULL ? node->next[0] : NULL) {
		if(node->num_next_nodes > 1) return 0;
		num_nodes++;
		num_steps += node->body != NULL;
		last = node;
	}
	if(num_steps < 3 || last->body == NULL) return 0;

	Body **bodies = malloc(num_steps * sizeof(*bodies));
	double *times = malloc(num_steps * sizeof(*times));
	PoweredLegDsm *dsms = calloc(num_steps - 1, sizeof(*dsms));
	OSV *states = calloc(num_steps, sizeof(*states));
	Lambert3 *legs = calloc(num_steps - 1, sizeof(*legs));
	PoweredDsmResult *dsm_results = calloc(num_steps - 1, sizeof(*dsm_results));
	int success = 0;
	if(bodies == NULL || times == NULL || dsms == NULL || states == NULL || legs == NULL || dsm_results == NULL)
		goto cleanup;

	size_t route_index = 0;
	for(struct ItinStep *node = first; node != NULL; node = node->next != NULL ? node->next[0] : NULL) {
		if(node->body == NULL) continue;
		bodies[route_index] = node->body;
		times[route_index] = node->date;
		route_index++;
	}
	for(size_t i = 0; i + 1 < num_steps; i++)
		if(!(times[i + 1] > times[i])) goto cleanup;

	route_index = 0;
	for(struct ItinStep *node = first; node != NULL; node = node->next != NULL ? node->next[0] : NULL) {
		if(node->body != NULL) { route_index++; continue; }
		size_t leg = route_index - 1;
		if(dsms[leg].enabled) goto cleanup;
		dsms[leg] = (PoweredLegDsm){.enabled = 1, .use_position = 1, .position = node->r, .date = node->date};
	}

	Itin_Calc_Data calc_data = {0};
	calc_data.jd_max_arr = times[num_steps - 1] + 1;
	calc_data.max_duration = times[num_steps - 1] - times[0] + 1;
	calc_data.dv_filter = (struct Dv_Filter){.max_totdv = 1e12, .max_depdv = 1e12, .max_satdv = 1e12,
		.dep_periapsis = dep_periapsis, .arr_periapsis = arr_periapsis, .last_transfer_type = last_transfer_type};
	calc_data.seq_info.to_target.system = system;

	PoweredRouteCandidate candidate = {.num_steps = num_steps, .bodies = bodies, .times = times, .dsms = dsms};

	// Start the flyby iteration from the stored, already converged velocities; fall back to plain arcs.
	Vector3 *seed_incoming = calloc(num_steps, sizeof(*seed_incoming));
	Vector3 *seed_outgoing = calloc(num_steps, sizeof(*seed_outgoing));
	int seeded = seed_incoming != NULL && seed_outgoing != NULL;
	size_t seed_index = 0;
	for(struct ItinStep *node = first; node != NULL && seeded; node = node->next != NULL ? node->next[0] : NULL) {
		if(node->body == NULL) continue;
		if(seed_index > 0 && seed_index + 1 < num_steps) {
			OSV body_state;
			if(node->next == NULL || !body_state_at(system, node->body, node->date, &body_state)) { seeded = 0; break; }
			seed_incoming[seed_index] = subtract_vec3(node->v_arr, body_state.v);
			seed_outgoing[seed_index] = subtract_vec3(node->next[0]->v_dep, body_state.v);
			if(!is_finite_vector(seed_incoming[seed_index]) || !is_finite_vector(seed_outgoing[seed_index]) ||
				!(mag_vec3(seed_incoming[seed_index]) > 0) || !(mag_vec3(seed_outgoing[seed_index]) > 0)) seeded = 0;
		}
		seed_index++;
	}
	// The flyby iteration can settle on a mirrored, costlier flyby orientation, so try both starting points and keep the cheaper.
	OSV *trial_states = calloc(num_steps, sizeof(*trial_states));
	Lambert3 *trial_legs = calloc(num_steps - 1, sizeof(*trial_legs));
	PoweredDsmResult *trial_dsm = calloc(num_steps - 1, sizeof(*trial_dsm));
	double resolved_dv = DBL_MAX;
	int resolved_converged = 0;
	for(int attempt = 0; attempt < 2 && trial_states != NULL && trial_legs != NULL && trial_dsm != NULL; attempt++) {
		if(attempt == 0 && !seeded) continue;
		candidate.seed_incoming = attempt == 0 ? seed_incoming : NULL;
		candidate.seed_outgoing = attempt == 0 ? seed_outgoing : NULL;
		int trial_converged = 0;
		candidate.accept_unconverged = accept_unconverged;
		candidate.converged_out = &trial_converged;
		double trial_dv = evaluate_route_core(&candidate, &calc_data, times, trial_states, trial_legs, trial_dsm);
		// A converged result always beats an unconverged one; otherwise keep the cheaper.
		int better = trial_dv < DBL_MAX && (resolved_dv >= DBL_MAX || (trial_converged && !resolved_converged) ||
			(trial_converged == resolved_converged && trial_dv < resolved_dv));
		if(better) {
			resolved_dv = trial_dv;
			resolved_converged = trial_converged;
			memcpy(states, trial_states, num_steps * sizeof(*states));
			memcpy(legs, trial_legs, (num_steps - 1) * sizeof(*legs));
			memcpy(dsm_results, trial_dsm, (num_steps - 1) * sizeof(*dsm_results));
		}
	}
	candidate.seed_incoming = candidate.seed_outgoing = NULL;
	candidate.converged_out = NULL;
	free(trial_states);
	free(trial_legs);
	free(trial_dsm);
	free(seed_incoming);
	free(seed_outgoing);
	if(resolved_dv >= DBL_MAX) goto cleanup;

	route_index = 0;
	for(struct ItinStep *node = first; node != NULL; node = node->next != NULL ? node->next[0] : NULL) {
		if(node->body == NULL) {
			size_t leg = route_index - 1;
			node->v_arr = dsm_results[leg].velocity_before;
			node->v_dep = legs[leg].v0;
			node->v_body = vec3(1, 0, 0);
			continue;
		}
		node->r = states[route_index].r;
		node->v_body = states[route_index].v;
		if(route_index > 0) {
			size_t leg = route_index - 1;
			node->v_arr = legs[leg].v1;
			node->v_dep = dsms[leg].enabled ? dsm_results[leg].velocity_after : legs[leg].v0;
		}
		route_index++;
	}
	success = 1;
	if(converged_out != NULL) *converged_out = resolved_converged;

cleanup:
	free(bodies);
	free(times);
	free(dsms);
	free(states);
	free(legs);
	free(dsm_results);
	(void)num_nodes;
	return success;
}

int resolve_itinerary_with_powered_flybys(CelestSystem *system, struct ItinStep *first,
		double dep_periapsis, double arr_periapsis, enum LastTransferType last_transfer_type) {
	return resolve_itinerary_internal(system, first, dep_periapsis, arr_periapsis, last_transfer_type, 0, NULL);
}

int resolve_itinerary_with_powered_flybys_approximate(CelestSystem *system, struct ItinStep *first,
		double dep_periapsis, double arr_periapsis, enum LastTransferType last_transfer_type, int *converged) {
	int strict_converged = 0;
	if(resolve_itinerary_internal(system, first, dep_periapsis, arr_periapsis, last_transfer_type, 0, &strict_converged)) {
		if(converged != NULL) *converged = 1;
		return 1;
	}
	return resolve_itinerary_internal(system, first, dep_periapsis, arr_periapsis, last_transfer_type, 1, converged);
}
