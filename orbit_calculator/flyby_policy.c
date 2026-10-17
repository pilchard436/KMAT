#include "flyby_policy.h"
#include <math.h>

// Per thread, so the planner can relax the limit without touching search workers.
static _Thread_local int unrestricted_flyby_periapsis;

int set_flyby_unrestricted_periapsis(int enable) {
	int previous = unrestricted_flyby_periapsis;
	unrestricted_flyby_periapsis = enable;
	return previous;
}

double flyby_minimum_periapsis(Body *body) {
	return unrestricted_flyby_periapsis ? body->radius * 0.1 : body->radius + body->atmo_alt + 1e4;
}

// Drawing asks about the same flybys every animation frame, and each solve scans many periapsis radii.
typedef struct {
	Vector3 v_arr, v_dep, v_body;
	Body *body;
	int unrestricted, feasible;
	double periapsis;
} FlybyCacheEntry;
static _Thread_local FlybyCacheEntry flyby_cache[32];
static _Thread_local int flyby_cache_next;

static int same_vec(Vector3 a, Vector3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

static int cached_powered_flyby(Vector3 v_arr, Vector3 v_dep, Vector3 v_body, Body *body, double *periapsis) {
	for(int i = 0; i < 32; i++) {
		FlybyCacheEntry *entry = &flyby_cache[i];
		if(entry->body == body && entry->unrestricted == unrestricted_flyby_periapsis &&
				same_vec(entry->v_arr, v_arr) && same_vec(entry->v_dep, v_dep) && same_vec(entry->v_body, v_body)) {
			if(periapsis != NULL) *periapsis = entry->periapsis;
			return entry->feasible;
		}
	}
	PoweredFlybyResult flyby;
	int feasible = calculate_powered_flyby(v_arr, v_dep, v_body, body, flyby_minimum_periapsis(body), &flyby);
	flyby_cache[flyby_cache_next] = (FlybyCacheEntry){v_arr, v_dep, v_body, body, unrestricted_flyby_periapsis,
		feasible, feasible ? flyby.periapsis_radius : 0};
	flyby_cache_next = (flyby_cache_next + 1) % 32;
	if(periapsis != NULL) *periapsis = feasible ? flyby.periapsis_radius : 0;
	return feasible;
}

double flyby_periapsis_shortfall(struct ItinStep *step) {
	if(step == NULL || step->body == NULL || step->prev == NULL || step->next == NULL) return 0;
	double minimum_radius = step->body->radius + step->body->atmo_alt;
	double unpowered = get_flyby_periapsis(step->v_arr, step->next[0]->v_dep, step->v_body, step->body);
	if(!isfinite(unpowered)) return 0;
	return unpowered < minimum_radius ? minimum_radius - unpowered : 0;
}

int flyby_below_min_safe(struct ItinStep *step) {
	if(step == NULL || step->body == NULL || step->prev == NULL || step->next == NULL) return 0;
	double safe_radius = step->body->radius + step->body->atmo_alt + 1e4;
	double flown;
	if(!cached_powered_flyby(step->v_arr, step->next[0]->v_dep, step->v_body, step->body, &flown))
		flown = get_flyby_periapsis(step->v_arr, step->next[0]->v_dep, step->v_body, step->body);
	return isfinite(flown) && flown < safe_radius - 1;
}

int flyby_is_viable(Vector3 v_arr, Vector3 v_dep, Vector3 v_body, Body *body) {
	return cached_powered_flyby(v_arr, v_dep, v_body, body, NULL);
}

int count_flybys_below_min_safe(struct ItinStep *step_in_route) {
	int count = 0;
	for(struct ItinStep *step = get_first(step_in_route); step != NULL; step = step->next != NULL ? step->next[0] : NULL)
		count += flyby_below_min_safe(step);
	return count;
}

int count_flybys_needing_burn(struct ItinStep *step_in_route, double *largest_shortfall) {
	int count = 0;
	double largest = 0;
	for(struct ItinStep *step = get_first(step_in_route); step != NULL; step = step->next != NULL ? step->next[0] : NULL) {
		double shortfall = flyby_periapsis_shortfall(step);
		if(shortfall > 0) {
			count++;
			largest = fmax(largest, shortfall);
		}
	}
	if(largest_shortfall != NULL) *largest_shortfall = largest;
	return count;
}
