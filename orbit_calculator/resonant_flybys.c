#include "resonant_flybys.h"
#include "flyby_policy.h"
#include "leg_variants.h"
#include "search_cancel.h"
#include <math.h>
#include <stdlib.h>

#define RESONANT_MAX_ROOTS 12
#define RESONANT_MAX_BODY_REVS 2
#define RESONANT_SAMPLE_SECONDS (6 * 86400.0)
#define RESONANT_DEPARTURE_STEPS 16

int resonant_departure_sample_count(void) {
	return (1 + 2 * 2 * 1) * RESONANT_DEPARTURE_STEPS + (1 + 2 * 2 * 2) * RESONANT_DEPARTURE_STEPS;
}

void resonant_departure_sample(int index, double period_days, double *duration_days, int *variant) {
	int first_group = (1 + 2 * 2 * 1) * RESONANT_DEPARTURE_STEPS;
	int body_revs = index < first_group ? 1 : 2;
	int local = index < first_group ? index : index - first_group;
	*variant = local / RESONANT_DEPARTURE_STEPS;
	*duration_days = body_revs * period_days * (0.75 + 0.5 * (local % RESONANT_DEPARTURE_STEPS + 0.5) / RESONANT_DEPARTURE_STEPS);
}

// Outgoing minus incoming excess speed for a return to the same body dt seconds later on the given leg variant.
static int resonant_speed_difference(struct ItinStep *tf, CelestSystem *system, double incoming_speed,
		double dt, int variant, Lambert3 *leg, double *difference) {
	double epoch = tf->date + dt / 86400;
	OSV target = system->prop_method == ORB_ELEMENTS ?
		osv_from_elements(tf->body->orbit, epoch) :
		osv_from_ephem(tf->body->ephem, tf->body->num_ephems, epoch, system->cb);
	if(!isfinite(target.r.x + target.r.y + target.r.z)) return 0;
	*leg = calc_leg_variant_lambert(system->cb, tf->r, target.r, dt, variant, itinerary_search_cancelled, NULL);
	if(itinerary_search_cancelled(NULL) || !is_usable_lambert(*leg)) return 0;
	*difference = mag_vec3(subtract_vec3(leg->v0, tf->v_body)) - incoming_speed;
	return isfinite(*difference);
}

void find_resonant_flybys(struct ItinStep *tf, CelestSystem *system, double min_dt, double max_dt) {
	if(tf->prev == NULL || tf->body == NULL) return;
	double incoming_speed = mag_vec3(subtract_vec3(tf->v_arr, tf->v_body));
	double period = calc_orbital_period(constr_orbit_from_osv(tf->r, tf->v_body, system->cb));
	if(!(incoming_speed > 0) || !(period > 0) || !isfinite(period)) return;
	double minimum_periapsis = flyby_minimum_periapsis(tf->body);
	struct ItinStep *new_steps[RESONANT_MAX_ROOTS];
	int counter = 0;
	for(int body_revs = 1; body_revs <= RESONANT_MAX_BODY_REVS && counter < RESONANT_MAX_ROOTS; body_revs++) {
		double window_start = fmax(min_dt, body_revs * period * 0.75);
		double window_end = fmin(max_dt, body_revs * period * 1.25);
		if(window_end <= window_start) continue;
		int variants = 1 + 2 * 2 * body_revs;
		if(variants > LEG_VARIANT_COUNT) variants = LEG_VARIANT_COUNT;
		for(int variant = 0; variant < variants && counter < RESONANT_MAX_ROOTS &&
			!itinerary_search_cancelled(NULL); variant++) {
			double previous_dt = 0, previous_difference = 0;
			int has_previous = 0;
			for(double dt = window_start; counter < RESONANT_MAX_ROOTS; dt += RESONANT_SAMPLE_SECONDS) {
				if(dt > window_end) dt = window_end;
				Lambert3 leg;
				double difference;
				int valid = resonant_speed_difference(tf, system, incoming_speed, dt, variant, &leg, &difference);
				if(valid && has_previous && previous_difference * difference < 0) {
					double low = previous_dt, high = dt, low_difference = previous_difference;
					Lambert3 root_leg = leg;
					double root_difference = difference;
					for(int iteration = 0; iteration < 40; iteration++) {
						double middle = (low + high) / 2;
						Lambert3 middle_leg;
						double middle_difference;
						if(!resonant_speed_difference(tf, system, incoming_speed, middle, variant, &middle_leg, &middle_difference)) break;
						root_leg = middle_leg;
						root_difference = middle_difference;
						if(fabs(middle_difference) < 1e-3) { low = high = middle; break; }
						if(low_difference * middle_difference < 0) high = middle;
						else { low = middle; low_difference = middle_difference; }
					}
					double root_dt = (low + high) / 2;
					PoweredFlybyResult flyby;
					Orbit leg_orbit = constr_orbit_from_osv(tf->r, root_leg.v0, system->cb);
					// A leg that dips into the central body, or a bisection that did not settle, is not a flyby.
					if(fabs(root_difference) < 1 && leg_orbit.e < 1 && leg_orbit.a * (1 - leg_orbit.e) > 2 * system->cb->radius &&
						calculate_powered_flyby(tf->v_arr, root_leg.v0, tf->v_body, tf->body, minimum_periapsis, &flyby) &&
						flyby.burn_delta_v < 1) {
						double epoch = tf->date + root_dt / 86400;
						int duplicate = 0;
						for(int i = 0; i < counter; i++) if(fabs(new_steps[i]->date - epoch) < 1e-6) duplicate = 1;
						struct ItinStep *new_step = duplicate ? NULL : calloc(1, sizeof(*new_step));
						if(new_step != NULL) {
							OSV target = system->prop_method == ORB_ELEMENTS ?
								osv_from_elements(tf->body->orbit, epoch) :
								osv_from_ephem(tf->body->ephem, tf->body->num_ephems, epoch, system->cb);
							new_step->body = tf->body;
							new_step->date = epoch;
							new_step->r = target.r;
							new_step->v_body = target.v;
							new_step->v_dep = root_leg.v0;
							new_step->v_arr = root_leg.v1;
							new_step->prev = tf;
							new_steps[counter++] = new_step;
						}
					}
				}
				previous_dt = dt;
				previous_difference = difference;
				has_previous = valid;
				if(dt >= window_end) break;
			}
		}
	}
	if(counter == 0) return;
	struct ItinStep **next = realloc(tf->next, (size_t)(tf->num_next_nodes + counter) * sizeof(*next));
	if(next == NULL) {
		for(int i = 0; i < counter; i++) free(new_steps[i]);
		return;
	}
	tf->next = next;
	for(int i = 0; i < counter; i++) tf->next[tf->num_next_nodes + i] = new_steps[i];
	tf->num_next_nodes += counter;
}
