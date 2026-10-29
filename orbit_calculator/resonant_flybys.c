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

// The transfer plane flips when the returning body passes through 0 or 180 degrees of transfer angle, which the
// resonant windows straddle; the Lambert solution and the speed difference jump there, so a sign change across the
// flip is not a root. Roots are refined only on the smooth side intervals, starting this far from the flip.
#define RESONANT_SINGULARITY_MARGIN_SECONDS 5.0
#define RESONANT_REFINE_ITERATIONS 40
#define RESONANT_SPEED_TOLERANCE 1e-3

typedef struct ResonantSearch {
	struct ItinStep *tf;
	CelestSystem *system;
	double incoming_speed, minimum_periapsis;
	struct ItinStep *new_steps[RESONANT_MAX_ROOTS];
	int counter;
} ResonantSearch;

static OSV resonant_target_state(const ResonantSearch *search, double epoch) {
	struct ItinStep *tf = search->tf;
	return search->system->prop_method == ORB_ELEMENTS ?
		osv_from_elements(tf->body->orbit, epoch) :
		osv_from_ephem(tf->body->ephem, tf->body->num_ephems, epoch, search->system->cb);
}

// Speed difference from an already solved leg; see resonant_speed_difference.
static int resonant_speed_difference_of_leg(const ResonantSearch *search, Lambert3 leg, double *difference) {
	if(itinerary_search_cancelled(NULL) || !is_usable_lambert(leg)) return 0;
	*difference = mag_vec3(subtract_vec3(leg.v0, search->tf->v_body)) - search->incoming_speed;
	return isfinite(*difference);
}

// Outgoing minus incoming excess speed for a return to the same body dt seconds later on the given leg variant.
static int resonant_speed_difference(const ResonantSearch *search, double dt, int variant, Lambert3 *leg, double *difference) {
	OSV target = resonant_target_state(search, search->tf->date + dt / 86400);
	if(!isfinite(target.r.x + target.r.y + target.r.z)) return 0;
	*leg = calc_leg_variant_lambert(search->system->cb, search->tf->r, target.r, dt, variant, itinerary_search_cancelled, NULL);
	return resonant_speed_difference_of_leg(search, *leg, difference);
}

// Sign of the transfer plane normal's z component for a return dt seconds later.
static int resonant_plane_sign(const ResonantSearch *search, double dt) {
	OSV target = resonant_target_state(search, search->tf->date + dt / 86400);
	return cross_vec3(search->tf->r, target.r).z < 0 ? -1 : 1;
}

// Stores the root as a resonant flyby step if the leg stays clear of the central body and the flyby is unpowered.
static void accept_resonant_root(ResonantSearch *search, double root_dt, Lambert3 root_leg, double root_difference) {
	struct ItinStep *tf = search->tf;
	PoweredFlybyResult flyby;
	Orbit leg_orbit = constr_orbit_from_osv(tf->r, root_leg.v0, search->system->cb);
	// A leg that dips into the central body, or a refinement that did not settle, is not a flyby.
	if(!(fabs(root_difference) < 1 && leg_orbit.e < 1 && leg_orbit.a * (1 - leg_orbit.e) > 2 * search->system->cb->radius &&
		calculate_powered_flyby(tf->v_arr, root_leg.v0, tf->v_body, tf->body, search->minimum_periapsis, &flyby) &&
		flyby.burn_delta_v < 1)) return;
	double epoch = tf->date + root_dt / 86400;
	for(int i = 0; i < search->counter; i++) if(fabs(search->new_steps[i]->date - epoch) < 1e-6) return;
	struct ItinStep *new_step = calloc(1, sizeof(*new_step));
	if(new_step == NULL) return;
	OSV target = resonant_target_state(search, epoch);
	new_step->body = tf->body;
	new_step->date = epoch;
	new_step->r = target.r;
	new_step->v_body = target.v;
	new_step->v_dep = root_leg.v0;
	new_step->v_arr = root_leg.v1;
	new_step->prev = tf;
	search->new_steps[search->counter++] = new_step;
}

// Refines a sign change of the speed difference on a smooth interval with the Illinois method (regula falsi that
// halves the retained endpoint's value when the same side is kept twice; bisection if the secant leaves the bracket).
static void refine_resonant_bracket(ResonantSearch *search, int variant, double low, double high,
		double low_difference, Lambert3 high_leg, double high_difference) {
	Lambert3 root_leg = high_leg;
	double root_difference = high_difference;
	int retained_side = 0;
	for(int iteration = 0; iteration < RESONANT_REFINE_ITERATIONS; iteration++) {
		double middle = (low_difference * high - high_difference * low) / (low_difference - high_difference);
		if(!(middle > low && middle < high)) middle = (low + high) / 2;
		Lambert3 middle_leg;
		double middle_difference;
		if(!resonant_speed_difference(search, middle, variant, &middle_leg, &middle_difference)) break;
		root_leg = middle_leg;
		root_difference = middle_difference;
		if(fabs(middle_difference) < RESONANT_SPEED_TOLERANCE) { low = high = middle; break; }
		if(low_difference * middle_difference < 0) {
			high = middle;
			high_difference = middle_difference;
			if(retained_side == -1) low_difference *= 0.5;
			retained_side = -1;
		} else {
			low = middle;
			low_difference = middle_difference;
			if(retained_side == 1) high_difference *= 0.5;
			retained_side = 1;
		}
	}
	accept_resonant_root(search, (low + high) / 2, root_leg, root_difference);
}

// A sign change that straddles the transfer-plane flip: locate the flip, then refine the smooth parts on either side.
// If neither side holds a root, the better side next to the flip is still offered as a near-exact-period return.
static void resolve_singular_bracket(ResonantSearch *search, int variant, double low, double high,
		double low_difference, Lambert3 high_leg, double high_difference, int low_sign) {
	double a = low, b = high;
	while(b - a > 1) {
		double middle = (a + b) / 2;
		if(resonant_plane_sign(search, middle) == low_sign) a = middle;
		else b = middle;
	}
	double flip = (a + b) / 2;
	double left_dt = flip - RESONANT_SINGULARITY_MARGIN_SECONDS;
	double right_dt = flip + RESONANT_SINGULARITY_MARGIN_SECONDS;
	Lambert3 left_leg, right_leg;
	double left_difference = 0, right_difference = 0;
	int left_valid = left_dt > low && resonant_speed_difference(search, left_dt, variant, &left_leg, &left_difference);
	int right_valid = right_dt < high && resonant_speed_difference(search, right_dt, variant, &right_leg, &right_difference);
	int found = 0;
	if(left_valid && low_difference * left_difference < 0) {
		refine_resonant_bracket(search, variant, low, left_dt, low_difference, left_leg, left_difference);
		found = 1;
	}
	if(right_valid && right_difference * high_difference < 0 && search->counter < RESONANT_MAX_ROOTS) {
		refine_resonant_bracket(search, variant, right_dt, high, right_difference, high_leg, high_difference);
		found = 1;
	}
	if(found || search->counter >= RESONANT_MAX_ROOTS) return;
	if(left_valid && (!right_valid || fabs(left_difference) <= fabs(right_difference)))
		accept_resonant_root(search, left_dt, left_leg, left_difference);
	else if(right_valid)
		accept_resonant_root(search, right_dt, right_leg, right_difference);
}

// A sign change of the speed difference between two consecutive scan samples of one leg variant.
typedef struct ResonantBracket {
	double low_dt, high_dt, low_difference, high_difference;
	Lambert3 high_leg;
	int singular, low_sign;
} ResonantBracket;

// Number of scan samples from window_start to window_end in RESONANT_SAMPLE_SECONDS steps, including the clamped end.
static int resonant_scan_sample_count(double window_start, double window_end) {
	int count = 0;
	for(double dt = window_start; ; dt += RESONANT_SAMPLE_SECONDS) {
		if(dt > window_end) dt = window_end;
		count++;
		if(dt >= window_end) break;
	}
	return count;
}

void find_resonant_flybys(struct ItinStep *tf, CelestSystem *system, double min_dt, double max_dt) {
	if(tf->prev == NULL || tf->body == NULL) return;
	ResonantSearch search = {.tf = tf, .system = system};
	search.incoming_speed = mag_vec3(subtract_vec3(tf->v_arr, tf->v_body));
	double period = calc_orbital_period(constr_orbit_from_osv(tf->r, tf->v_body, system->cb));
	if(!(search.incoming_speed > 0) || !(period > 0) || !isfinite(period)) return;
	search.minimum_periapsis = flyby_minimum_periapsis(tf->body);
	for(int body_revs = 1; body_revs <= RESONANT_MAX_BODY_REVS && search.counter < RESONANT_MAX_ROOTS; body_revs++) {
		double window_start = fmax(min_dt, body_revs * period * 0.75);
		double window_end = fmin(max_dt, body_revs * period * 1.25);
		if(window_end <= window_start) continue;
		int variants = 1 + 2 * 2 * body_revs;
		if(variants > LEG_VARIANT_COUNT) variants = LEG_VARIANT_COUNT;
		if(itinerary_search_cancelled(NULL)) break;

		// Scan every sample date once for all variants, since the target state does not depend on the variant.
		// Brackets are kept per variant in date order, so roots are refined in the order a variant-by-variant
		// scan would find them.
		int samples = resonant_scan_sample_count(window_start, window_end);
		ResonantBracket *brackets = malloc((size_t)variants * (size_t)samples * sizeof(*brackets));
		if(brackets == NULL) break;
		int bracket_counts[LEG_VARIANT_COUNT] = {0};
		double previous_dt[LEG_VARIANT_COUNT] = {0}, previous_difference[LEG_VARIANT_COUNT] = {0};
		int has_previous[LEG_VARIANT_COUNT] = {0};
		int previous_sign = 0;
		for(double dt = window_start; !itinerary_search_cancelled(NULL); dt += RESONANT_SAMPLE_SECONDS) {
			if(dt > window_end) dt = window_end;
			OSV target = resonant_target_state(&search, tf->date + dt / 86400);
			int sign = cross_vec3(tf->r, target.r).z < 0 ? -1 : 1;
			Lambert3 legs[LEG_VARIANT_COUNT];
			int target_valid = isfinite(target.r.x + target.r.y + target.r.z);
			if(target_valid) calc_leg_variant_lamberts(system->cb, tf->r, target.r, dt, variants, legs, itinerary_search_cancelled, NULL);
			for(int variant = 0; variant < variants; variant++) {
				double difference = 0;
				int valid = target_valid && resonant_speed_difference_of_leg(&search, legs[variant], &difference);
				if(valid && has_previous[variant] && previous_difference[variant] * difference < 0) {
					brackets[variant * samples + bracket_counts[variant]++] = (ResonantBracket){
						.low_dt = previous_dt[variant], .high_dt = dt, .low_difference = previous_difference[variant],
						.high_difference = difference, .high_leg = legs[variant], .singular = sign != previous_sign, .low_sign = previous_sign};
				}
				previous_dt[variant] = dt;
				previous_difference[variant] = difference;
				has_previous[variant] = valid;
			}
			previous_sign = sign;
			if(dt >= window_end) break;
		}

		for(int variant = 0; variant < variants && search.counter < RESONANT_MAX_ROOTS && !itinerary_search_cancelled(NULL); variant++) {
			for(int b = 0; b < bracket_counts[variant] && search.counter < RESONANT_MAX_ROOTS; b++) {
				ResonantBracket bracket = brackets[variant * samples + b];
				if(bracket.singular)
					resolve_singular_bracket(&search, variant, bracket.low_dt, bracket.high_dt, bracket.low_difference,
						bracket.high_leg, bracket.high_difference, bracket.low_sign);
				else
					refine_resonant_bracket(&search, variant, bracket.low_dt, bracket.high_dt, bracket.low_difference,
						bracket.high_leg, bracket.high_difference);
			}
		}
		free(brackets);
	}
	if(search.counter == 0) return;
	struct ItinStep **next = realloc(tf->next, (size_t)(tf->num_next_nodes + search.counter) * sizeof(*next));
	if(next == NULL) {
		for(int i = 0; i < search.counter; i++) free(search.new_steps[i]);
		return;
	}
	tf->next = next;
	for(int i = 0; i < search.counter; i++) tf->next[tf->num_next_nodes + i] = search.new_steps[i];
	tf->num_next_nodes += search.counter;
}
