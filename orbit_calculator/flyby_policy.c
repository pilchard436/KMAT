#include "flyby_policy.h"
#include "moon_flyby.h"
#include <math.h>
#include <string.h>

#define MIN_ASSISTED_BURN 1.0
#define DEFAULT_MINIMUM_LEG_DAYS 10.0
#define MINIMUM_LEG_PERIOD_FRACTION 0.125
#define MINIMUM_LEG_FLOOR_DAYS 0.05

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

// Search nodes keep the plain burn; the final scoring and the planner use moon assists.
static _Thread_local int moon_assist_enabled;

int set_moon_assist_enabled(int enable) {
	int previous = moon_assist_enabled;
	moon_assist_enabled = enable;
	return previous;
}

int moon_assist_is_active(void) {
	return moon_assist_enabled || unrestricted_flyby_periapsis;
}

double arrival_dv_with_assist(Body *body, Vector3 incoming_excess, double arrival_radius, int circularize, double plain_dv,
		double jd, MoonAssist *assist) {
	if(assist != NULL) assist->moon = NULL;
	if(plain_dv < MIN_ASSISTED_BURN) return plain_dv;
	MoonAssist found;
	if(!moon_assist_arrival(body, incoming_excess, arrival_radius, circularize ? dv_circ : dv_capture, plain_dv, jd, &found))
		return plain_dv;
	if(assist != NULL) *assist = found;
	return found.total_dv;
}

int calculate_flyby_with_moons(Vector3 v_arr, Vector3 v_dep, Vector3 v_body, Body *body,
		double minimum_periapsis, double flyby_jd, PoweredFlybyResult *result) {
	if(!calculate_powered_flyby(v_arr, v_dep, v_body, body, minimum_periapsis, result)) return 0;
	MoonFlybyEffect effect;
	if(!analyze_moon_flyby(body, result, flyby_jd, unrestricted_flyby_periapsis, &effect)) return 1;
	// The planner relaxes the limits to show what is wrong, so it keeps flybys that hit a moon.
	if(effect.hits_moon && !unrestricted_flyby_periapsis) return 0;
	if(effect.integrated) result->burn_delta_v += mag_vec3(effect.velocity_error);
	return 1;
}

int flyby_blocked_by_moon(Body *body, const PoweredFlybyResult *flyby, double flyby_jd) {
	MoonFlybyEffect effect;
	return !unrestricted_flyby_periapsis && analyze_moon_flyby(body, flyby, flyby_jd, 0, &effect) &&
		(effect.hits_moon || effect.perturbing_moon != NULL);
}

Body * flyby_moon_hit(struct ItinStep *step) {
	if(step == NULL || step->body == NULL || step->prev == NULL || step->next == NULL) return NULL;
	PoweredFlybyResult flyby;
	if(!calculate_powered_flyby(step->v_arr, step->next[0]->v_dep, step->v_body, step->body,
		flyby_minimum_periapsis(step->body), &flyby)) return NULL;
	MoonFlybyEffect effect;
	return analyze_moon_flyby(step->body, &flyby, step->date, 0, &effect) && effect.hits_moon ? effect.moon_hit : NULL;
}

// Drawing asks about the same flybys every animation frame, and each solve scans many periapsis radii.
typedef struct {
	Vector3 v_arr, v_dep, v_body;
	Body *body;
	double jd;
	int unrestricted, feasible;
	double periapsis;
} FlybyCacheEntry;
static _Thread_local FlybyCacheEntry flyby_cache[32];
static _Thread_local int flyby_cache_next;

static int same_vec(Vector3 a, Vector3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

static int cached_powered_flyby(Vector3 v_arr, Vector3 v_dep, Vector3 v_body, Body *body, double jd, double *periapsis) {
	for(int i = 0; i < 32; i++) {
		FlybyCacheEntry *entry = &flyby_cache[i];
		if(entry->body == body && entry->jd == jd && entry->unrestricted == unrestricted_flyby_periapsis &&
				same_vec(entry->v_arr, v_arr) && same_vec(entry->v_dep, v_dep) && same_vec(entry->v_body, v_body)) {
			if(periapsis != NULL) *periapsis = entry->periapsis;
			return entry->feasible;
		}
	}
	PoweredFlybyResult flyby;
	int feasible = calculate_flyby_with_moons(v_arr, v_dep, v_body, body, flyby_minimum_periapsis(body), jd, &flyby);
	flyby_cache[flyby_cache_next] = (FlybyCacheEntry){v_arr, v_dep, v_body, body, jd, unrestricted_flyby_periapsis,
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
	if(!cached_powered_flyby(step->v_arr, step->next[0]->v_dep, step->v_body, step->body, step->date, &flown))
		flown = get_flyby_periapsis(step->v_arr, step->next[0]->v_dep, step->v_body, step->body);
	return (isfinite(flown) && flown < safe_radius - 1) || flyby_moon_hit(step) != NULL;
}

int flyby_is_viable(Vector3 v_arr, Vector3 v_dep, Vector3 v_body, Body *body, double jd) {
	return cached_powered_flyby(v_arr, v_dep, v_body, body, jd, NULL);
}

// Leaves of one subtree share their earlier flybys, so the verdicts are kept in a small cache.
typedef struct {
	Body *body;
	Vector3 v_arr, v_dep;
	double jd;
	int blocked, used;
} MoonVerdict;
static _Thread_local MoonVerdict moon_verdicts[1024];

static int same_vector(Vector3 a, Vector3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

static int flyby_step_blocked_by_moon(struct ItinStep *flyby, struct ItinStep *next) {
	unsigned long long key;
	memcpy(&key, &flyby->date, sizeof(key));
	MoonVerdict *entry = &moon_verdicts[(key ^ (key >> 29) ^ ((size_t)flyby->body >> 4)) % 1024];
	if(entry->used && entry->body == flyby->body && entry->jd == flyby->date &&
		same_vector(entry->v_arr, flyby->v_arr) && same_vector(entry->v_dep, next->v_dep)) return entry->blocked;
	PoweredFlybyResult result;
	int blocked = calculate_powered_flyby(flyby->v_arr, next->v_dep, flyby->v_body, flyby->body,
		flyby_minimum_periapsis(flyby->body), &result) && flyby_blocked_by_moon(flyby->body, &result, flyby->date);
	*entry = (MoonVerdict){flyby->body, flyby->v_arr, next->v_dep, flyby->date, blocked, 1};
	return blocked;
}

int route_blocked_by_moons(struct ItinStep *arrival) {
	for(struct ItinStep *next = arrival; next->prev != NULL && next->prev->prev != NULL; next = next->prev)
		if(next->prev->body != NULL && flyby_step_blocked_by_moon(next->prev, next)) return 1;
	return 0;
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

double minimum_leg_days(const CelestSystem *system) {
	static _Thread_local const CelestSystem *cached_system;
	static _Thread_local int cached_body_count;
	static _Thread_local double cached_days;
	if(system == NULL || system->cb == NULL || !(system->cb->mu > 0)) return DEFAULT_MINIMUM_LEG_DAYS;
	if(system == cached_system && system->num_bodies == cached_body_count) return cached_days;
	double shortest_period = HUGE_VAL;
	for(int i = 0; i < system->num_bodies; i++) {
		const Body *body = system->bodies[i];
		if(body == NULL || !(body->orbit.a > 0) || !(body->orbit.e < 1)) continue;
		double period = 2 * M_PI * sqrt(body->orbit.a * body->orbit.a * body->orbit.a / system->cb->mu);
		if(period < shortest_period) shortest_period = period;
	}
	double days = DEFAULT_MINIMUM_LEG_DAYS;
	if(isfinite(shortest_period)) days = fmin(days, fmax(MINIMUM_LEG_FLOOR_DAYS, shortest_period / 86400 * MINIMUM_LEG_PERIOD_FRACTION));
	cached_system = system;
	cached_body_count = system->num_bodies;
	cached_days = days;
	return days;
}
