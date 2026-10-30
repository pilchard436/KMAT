#include "flyby_policy.h"
#include "moon_flyby.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MIN_ASSISTED_BURN 1.0
#define DEFAULT_MINIMUM_LEG_DAYS 10.0
#define MINIMUM_LEG_PERIOD_FRACTION 0.125
#define MINIMUM_LEG_FLOOR_DAYS 0.05

double flyby_minimum_periapsis(Body *body) {
	return body->radius + body->atmo_alt + 1e4;
}

// Moon assists are opt-in for arrival costs and independent of planner flyby constraints.
static _Thread_local int moon_assist_enabled;
static _Thread_local int moon_aware_flybys_enabled;

int set_moon_assist_enabled(int enable) {
	int previous = moon_assist_enabled;
	moon_assist_enabled = enable;
	return previous;
}

int moon_assist_is_active(void) {
	return moon_assist_enabled;
}

int set_moon_aware_flybys_enabled(int enable) {
	int previous = moon_aware_flybys_enabled;
	moon_aware_flybys_enabled = !!enable;
	return previous;
}

int moon_aware_flybys_is_active(void) {
	return moon_aware_flybys_enabled;
}

double arrival_dv_with_assist(Body *body, Vector3 incoming_excess, double arrival_radius, int circularize, double plain_dv,
		double jd, MoonAssist *assist) {
	if(assist != NULL) assist->moon = NULL;
	if(!moon_assist_is_active() || plain_dv < MIN_ASSISTED_BURN) return plain_dv;
	MoonAssist found;
	if(!moon_assist_arrival(body, incoming_excess, arrival_radius, circularize ? dv_circ : dv_capture, plain_dv, jd, &found))
		return plain_dv;
	if(assist != NULL) *assist = found;
	return found.total_dv;
}

#define REPLAN_RADII 96
#define REPLAN_MAX_ANALYSES 16

typedef struct {
	double radius, burn;
	int side;
} FlybyOption;

static int compare_options(const void *a, const void *b) {
	double first = ((const FlybyOption *)a)->burn, second = ((const FlybyOption *)b)->burn;
	return (first > second) - (first < second);
}

typedef struct {
	Body *body;
	Vector3 v_arr, v_dep, v_body;
	double jd, minimum_periapsis;
	int used, found;
	PoweredFlybyResult result;
	double radius;
	int side;
} ReplanEntry;
static _Thread_local ReplanEntry replan_cache[256];

static int same_excess(Vector3 a, Vector3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

int plan_flyby_around_moons(Vector3 v_arr, Vector3 v_dep, Vector3 v_body, Body *body, double minimum_periapsis,
		double jd, PoweredFlybyResult *result, double *radius, int *side) {
	unsigned long long key;
	memcpy(&key, &jd, sizeof(key));
	ReplanEntry *entry = &replan_cache[(key ^ (key >> 29) ^ ((size_t)body >> 4)) % 256];
	if(!(entry->used && entry->body == body && entry->jd == jd && entry->minimum_periapsis == minimum_periapsis &&
		same_excess(entry->v_arr, v_arr) && same_excess(entry->v_dep, v_dep) && same_excess(entry->v_body, v_body))) {
		*entry = (ReplanEntry){.body = body, .v_arr = v_arr, .v_dep = v_dep, .v_body = v_body, .jd = jd,
			.minimum_periapsis = minimum_periapsis, .used = 1};
		if(body != NULL && body->orbit.cb != NULL && body->orbit.cb->mu > 0 && body->orbit.a != 0) {
			double maximum = 0.95 * fabs(body->orbit.a) * pow(body->mu / body->orbit.cb->mu, 0.4);
			FlybyOption options[2 * REPLAN_RADII];
			int count = 0;
			for(int i = 0; maximum > minimum_periapsis && i < REPLAN_RADII; i++) {
				double periapsis = minimum_periapsis * pow(maximum / minimum_periapsis, (double)i / (REPLAN_RADII - 1));
				for(int s = -1; s <= 1; s += 2) {
					PoweredFlybyResult candidate;
					if(calculate_powered_flyby_at_periapsis(v_arr, v_dep, v_body, body, periapsis, s, &candidate))
						options[count++] = (FlybyOption){periapsis, candidate.burn_delta_v, s};
				}
			}
			qsort(options, count, sizeof(options[0]), compare_options);
			// The cheapest option that clears every moon; only the cheapest few are worth the moon scan.
			for(int i = 0; i < count && i < REPLAN_MAX_ANALYSES && !entry->found; i++) {
				PoweredFlybyResult candidate;
				MoonFlybyEffect effect;
				if(!calculate_powered_flyby_at_periapsis(v_arr, v_dep, v_body, body, options[i].radius, options[i].side, &candidate) ||
					!analyze_moon_flyby(body, &candidate, jd, 0, &effect)) break;
				if(effect.hits_moon || effect.perturbing_moon != NULL) continue;
				entry->found = 1;
				entry->result = candidate;
				entry->radius = options[i].radius;
				entry->side = options[i].side;
			}
		}
	}
	if(!entry->found) return 0;
	if(result != NULL) *result = entry->result;
	if(radius != NULL) *radius = entry->radius;
	if(side != NULL) *side = entry->side;
	return 1;
}

int calculate_flyby_with_moons(Vector3 v_arr, Vector3 v_dep, Vector3 v_body, Body *body,
		double minimum_periapsis, double flyby_jd, PoweredFlybyResult *result) {
	if(!calculate_powered_flyby(v_arr, v_dep, v_body, body, minimum_periapsis, result)) return 0;
	if(!moon_aware_flybys_enabled) return 1;
	MoonFlybyEffect effect;
	if(!analyze_moon_flyby(body, result, flyby_jd, 0, &effect)) return 1;
	if(effect.hits_moon || effect.perturbing_moon != NULL) {
		if(plan_flyby_around_moons(v_arr, v_dep, v_body, body, minimum_periapsis, flyby_jd, result, NULL, NULL)) return 1;
		if(effect.hits_moon) return 0;
	}
	return 1;
}

int flyby_blocked_by_moon(Body *body, const PoweredFlybyResult *flyby, double flyby_jd) {
	if(!moon_aware_flybys_is_active()) return 0;
	MoonFlybyEffect effect;
	return analyze_moon_flyby(body, flyby, flyby_jd, 0, &effect) &&
		(effect.hits_moon || effect.perturbing_moon != NULL);
}

Body * flyby_moon_hit(struct ItinStep *step) {
	if(step == NULL || step->body == NULL || step->prev == NULL || step->next == NULL) return NULL;
	PoweredFlybyResult flyby;
	if(!calculate_powered_flyby(step->v_arr, step->next[0]->v_dep, step->v_body, step->body,
		flyby_minimum_periapsis(step->body), &flyby)) return NULL;
	MoonFlybyEffect effect;
	if(!analyze_moon_flyby(step->body, &flyby, step->date, 0, &effect) || !effect.hits_moon) return NULL;
	return plan_flyby_around_moons(step->v_arr, step->next[0]->v_dep, step->v_body, step->body,
		flyby_minimum_periapsis(step->body), step->date, NULL, NULL, NULL) ? NULL : effect.moon_hit;
}

// Drawing asks about the same flybys every animation frame, and each solve scans many periapsis radii.
typedef struct {
	Vector3 v_arr, v_dep, v_body;
	Body *body;
	double jd;
	int moon_aware, feasible;
	double periapsis;
} FlybyCacheEntry;
static _Thread_local FlybyCacheEntry flyby_cache[32];
static _Thread_local int flyby_cache_next;

static int same_vec(Vector3 a, Vector3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

static int cached_powered_flyby(Vector3 v_arr, Vector3 v_dep, Vector3 v_body, Body *body, double jd, double *periapsis) {
	for(int i = 0; i < 32; i++) {
		FlybyCacheEntry *entry = &flyby_cache[i];
		if(entry->body == body && entry->jd == jd && entry->moon_aware == moon_aware_flybys_enabled &&
				same_vec(entry->v_arr, v_arr) && same_vec(entry->v_dep, v_dep) && same_vec(entry->v_body, v_body)) {
			if(periapsis != NULL) *periapsis = entry->periapsis;
			return entry->feasible;
		}
	}
	PoweredFlybyResult flyby;
	int feasible = calculate_flyby_with_moons(v_arr, v_dep, v_body, body, flyby_minimum_periapsis(body), jd, &flyby);
	flyby_cache[flyby_cache_next] = (FlybyCacheEntry){v_arr, v_dep, v_body, body, jd, moon_aware_flybys_enabled,
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
		flyby_minimum_periapsis(flyby->body), &result) && flyby_blocked_by_moon(flyby->body, &result, flyby->date) &&
		!plan_flyby_around_moons(flyby->v_arr, next->v_dep, flyby->v_body, flyby->body,
			flyby_minimum_periapsis(flyby->body), flyby->date, NULL, NULL, NULL);
	*entry = (MoonVerdict){flyby->body, flyby->v_arr, next->v_dep, flyby->date, blocked, 1};
	return blocked;
}

int route_blocked_by_moons(struct ItinStep *arrival) {
	if(!moon_aware_flybys_is_active()) return 0;
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

static int max_flybys_per_route = MAX_FLYBYS_PER_ROUTE;

int set_max_flybys_per_route(int flybys) {
	int previous = max_flybys_per_route;
	if(flybys >= 1) max_flybys_per_route = flybys > MAX_FLYBYS_PER_ROUTE ? MAX_FLYBYS_PER_ROUTE : flybys;
	return previous;
}

int get_max_flybys_per_route(void) {
	return max_flybys_per_route;
}

int route_flyby_count(const struct ItinStep *step) {
	int count = 0;
	for(; step != NULL && step->prev != NULL; step = step->prev) count++;
	return count;
}

static double orbital_period_seconds(const Body *body, const CelestSystem *system) {
	if(body == NULL || !(body->orbit.a > 0) || !(body->orbit.e < 1)) return NAN;
	return 2 * M_PI * sqrt(body->orbit.a * body->orbit.a * body->orbit.a / system->cb->mu);
}

double maximum_intermediate_leg_days(double from_radius, const Body *from, const Body *to, const CelestSystem *system) {
	if(system == NULL || system->cb == NULL || !(system->cb->mu > 0) || from == NULL || to == NULL || from == to ||
		!(from_radius > 0) || !(to->orbit.a > 0)) return HUGE_VAL;
	double period_from = orbital_period_seconds(from, system), period_to = orbital_period_seconds(to, system);
	if(!isfinite(period_from) || !isfinite(period_to)) return HUGE_VAL;
	double rate_difference = fabs(1 / period_from - 1 / period_to);
	double synodic = rate_difference > 0 ? 1 / rate_difference : fmax(period_from, period_to);
	Hohmann hohmann = calc_hohmann_transfer(from_radius, to->orbit.a, system->cb);
	double seconds = fmax(LEG_HORIZON_SYNODIC_PERIODS * synodic, LEG_HORIZON_HOHMANN_DURATIONS * hohmann.dur);
	return isfinite(seconds) && seconds > 0 ? seconds / 86400 : HUGE_VAL;
}
