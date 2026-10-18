#include "moon_flyby.h"
#include <math.h>

#define MOON_CLEARANCE 1e4
#define MAX_SCAN_SAMPLES 120
#define MIN_SCAN_STEP 1e6
#define MAX_INTEGRATION_STEPS 20000
#define MAX_TRACKED_MOONS 32

typedef struct {
	Vector3 periapsis_direction, tangent;
	double mu, excess_speed, periapsis, soi_time;
	double eccentricity, semi_major_axis, mean_motion;
	int outgoing;
} HyperbolaLeg;

int moon_state_at(Body *planet, Body *moon, double jd, OSV *state) {
	if(planet->system == NULL) return 0;
	if(planet->system->prop_method == ORB_ELEMENTS) {
		*state = osv_from_elements(moon->orbit, jd);
	} else {
		if(moon->ephem == NULL || moon->num_ephems <= 0) return 0;
		*state = osv_from_ephem(moon->ephem, moon->num_ephems, jd, planet);
	}
	return isfinite(state->r.x) && isfinite(state->r.y) && isfinite(state->r.z) &&
		isfinite(state->v.x) && isfinite(state->v.y) && isfinite(state->v.z);
}

static void init_leg(HyperbolaLeg *leg, const PoweredFlybyResult *flyby, double mu, int outgoing) {
	leg->mu = mu;
	leg->outgoing = outgoing;
	leg->periapsis = flyby->periapsis_radius;
	leg->periapsis_direction = flyby->periapsis_direction;
	leg->tangent = outgoing ? flyby->outgoing_tangent_direction : flyby->incoming_tangent_direction;
	OSV soi = outgoing ? flyby->outgoing_soi_state : flyby->incoming_soi_state;
	leg->soi_time = outgoing ? flyby->outgoing_soi_time : flyby->incoming_soi_time;
	double energy = 0.5 * dot_vec3(soi.v, soi.v) - mu / mag_vec3(soi.r);
	leg->excess_speed = sqrt(fmax(0, 2 * energy));
	leg->eccentricity = 1 + leg->periapsis * leg->excess_speed * leg->excess_speed / mu;
	leg->semi_major_axis = mu / (leg->excess_speed * leg->excess_speed);
	leg->mean_motion = sqrt(mu / pow(leg->semi_major_axis, 3));
}

// Time since periapsis for hyperbolic anomaly h (negative before periapsis).
static double leg_time(const HyperbolaLeg *leg, double h) {
	return (leg->eccentricity * sinh(h) - h) / leg->mean_motion;
}

static Vector3 leg_position(const HyperbolaLeg *leg, double h) {
	double e = leg->eccentricity, a = leg->semi_major_axis;
	double x = a * (e - cosh(h)), y = a * sqrt(e * e - 1) * sinh(h);
	return add_vec3(scale_vec3(leg->periapsis_direction, x), scale_vec3(leg->tangent, y));
}

static Vector3 leg_velocity(const HyperbolaLeg *leg, double h) {
	double e = leg->eccentricity, a = leg->semi_major_axis;
	double rate = leg->mean_motion / (e * cosh(h) - 1);
	Vector3 along_periapsis = scale_vec3(leg->periapsis_direction, -a * sinh(h));
	Vector3 along_tangent = scale_vec3(leg->tangent, a * sqrt(e * e - 1) * cosh(h));
	return scale_vec3(add_vec3(along_periapsis, along_tangent), rate);
}

static double leg_anomaly_at_radius(const HyperbolaLeg *leg, double radius) {
	double c = (radius / leg->semi_major_axis + 1) / leg->eccentricity;
	return c <= 1 ? 0 : acosh(c);
}

static double leg_anomaly_from_time(const HyperbolaLeg *leg, double t) {
	double mean = leg->mean_motion * t, e = leg->eccentricity;
	double h = asinh(mean / e);
	for(int i = 0; i < 30; i++) {
		double step = (e * sinh(h) - h - mean) / (e * cosh(h) - 1);
		h -= step;
		if(fabs(step) < 1e-12) break;
	}
	return h;
}

static double moon_clear_radius(const Body *moon) {
	return moon->radius + moon->atmo_alt + MOON_CLEARANCE;
}

// Moon radius, speed and orbit-plane normal change slowly, so one propagation serves a ten-day span.
typedef struct {
        Body *moon;
        void *ephem;
        double jd, a, inclination, raan, radius, speed;
        Vector3 normal;
} MoonFrame;
static _Thread_local MoonFrame moon_frames[64];
static _Thread_local int moon_frame_next;

static int moon_frame(Body *planet, Body *moon, double jd, MoonFrame *out) {
        for(int i = 0; i < 64; i++) {
                const MoonFrame *cached = &moon_frames[i];
                if(cached->moon == moon && cached->ephem == moon->ephem && cached->a == moon->orbit.a &&
                        cached->inclination == moon->orbit.i && cached->raan == moon->orbit.raan && fabs(cached->jd - jd) < 10) {
                        *out = *cached;
                        return 1;
                }
        }
        OSV state;
        if(!moon_state_at(planet, moon, jd, &state)) return 0;
        *out = (MoonFrame){moon, moon->ephem, jd, moon->orbit.a, moon->orbit.i, moon->orbit.raan,
                mag_vec3(state.r), mag_vec3(state.v), norm_vec3(cross_vec3(state.r, state.v))};
        moon_frames[moon_frame_next] = *out;
        moon_frame_next = (moon_frame_next + 1) % 64;
        return 1;
}

typedef struct {
	double distance, time; // time is signed seconds from periapsis
	Vector3 position, velocity, moon_velocity;
} ScanHit;

// Closest distance from the planned leg to a moon, scanning only while the leg is inside the moon's orbital shell widened by margin.
static double scan_leg(Body *planet, Body *moon, const HyperbolaLeg *leg, double flyby_jd,
		double margin, double step_distance, double moon_radius, double moon_speed, Vector3 plane_normal, double plane_clearance, ScanHit *hit) {
	if(hit != NULL) hit->distance = HUGE_VAL;
	double lo = moon_radius - margin, hi = moon_radius + margin;
	double rp = leg->periapsis;
	if(hi < rp) return HUGE_VAL;
	double h_lo = lo <= rp ? 0 : leg_anomaly_at_radius(leg, lo);
	double h_hi = leg_anomaly_at_radius(leg, hi);
	double t_lo = leg_time(leg, h_lo), t_hi = leg_time(leg, h_hi);
	if(t_hi > leg->soi_time) {
		t_hi = leg->soi_time;
		h_hi = leg_anomaly_from_time(leg, t_hi);
	}
	if(t_lo >= t_hi) return HUGE_VAL;
	// A leg that stays clear of the plane the moon moves in cannot reach it.
	int far_above = 1, far_below = 1;
	for(int i = 0; i < 5; i++) {
		double h = h_lo + (h_hi - h_lo) * i / 4;
		double height = dot_vec3(leg_position(leg, leg->outgoing ? h : -h), plane_normal);
		far_above &= height > plane_clearance;
		far_below &= height < -plane_clearance;
	}
	if(far_above || far_below) return HUGE_VAL;
	double speed = sqrt(leg->excess_speed * leg->excess_speed + 2 * leg->mu / fmax(lo, rp)) + moon_speed;
	double dt = fmax(step_distance, MIN_SCAN_STEP) / speed;
	int count = (int)ceil((t_hi - t_lo) / dt) + 1;
	if(count > MAX_SCAN_SAMPLES) count = MAX_SCAN_SAMPLES;
	if(count < 2) count = 2;
	double best = HUGE_VAL;
	Vector3 previous = vec3(0, 0, 0);
	for(int i = 0; i < count; i++) {
		double t = t_lo + (t_hi - t_lo) * i / (count - 1);
		double h = leg_anomaly_from_time(leg, t);
		double signed_t = leg->outgoing ? t : -t;
		OSV state;
		if(!moon_state_at(planet, moon, flyby_jd + signed_t / 86400, &state)) return HUGE_VAL;
		double signed_h = leg->outgoing ? h : -h;
		Vector3 position = leg_position(leg, signed_h);
		Vector3 relative = subtract_vec3(position, state.r);
		double distance = mag_vec3(relative);
		if(distance < best) best = distance;
		if(hit != NULL && distance < hit->distance)
			*hit = (ScanHit){distance, signed_t, position, leg_velocity(leg, signed_h), state.v};
		if(i > 0) {
			// Relative motion is near-linear over a step, so the segment's closest point is a better estimate than its ends.
			Vector3 step = subtract_vec3(relative, previous);
			double length2 = dot_vec3(step, step);
			double s = length2 > 0 ? fmax(0, fmin(1, -dot_vec3(previous, step) / length2)) : 0;
			double segment = mag_vec3(add_vec3(previous, scale_vec3(step, s)));
			if(segment < best) best = segment;
		}
		previous = relative;
	}
	return best;
}

typedef struct {
	Body *planet;
	Body *moons[MAX_TRACKED_MOONS];
	int count;
} MoonSet;

static Vector3 gravity_acceleration(const MoonSet *set, double jd, Vector3 r, double *nearest_moon_distance) {
	double radius = mag_vec3(r);
	Vector3 a = scale_vec3(r, -set->planet->mu / (radius * radius * radius));
	double nearest = HUGE_VAL;
	for(int i = 0; i < set->count; i++) {
		OSV moon;
		if(!moon_state_at(set->planet, set->moons[i], jd, &moon)) continue;
		Vector3 d = subtract_vec3(moon.r, r);
		double dist = mag_vec3(d), moon_radius = mag_vec3(moon.r);
		a = add_vec3(a, scale_vec3(d, set->moons[i]->mu / (dist * dist * dist)));
		a = subtract_vec3(a, scale_vec3(moon.r, set->moons[i]->mu / (moon_radius * moon_radius * moon_radius)));
		if(dist < nearest) nearest = dist;
	}
	if(nearest_moon_distance != NULL) *nearest_moon_distance = nearest;
	return a;
}

static void rk4_step(const MoonSet *set, double jd, double h, Vector3 *r, Vector3 *v) {
	double day = h / 86400;
	Vector3 k1r = *v, k1v = gravity_acceleration(set, jd, *r, NULL);
	Vector3 r2 = add_vec3(*r, scale_vec3(k1r, h / 2)), v2 = add_vec3(*v, scale_vec3(k1v, h / 2));
	Vector3 k2r = v2, k2v = gravity_acceleration(set, jd + day / 2, r2, NULL);
	Vector3 r3 = add_vec3(*r, scale_vec3(k2r, h / 2)), v3 = add_vec3(*v, scale_vec3(k2v, h / 2));
	Vector3 k3r = v3, k3v = gravity_acceleration(set, jd + day / 2, r3, NULL);
	Vector3 r4 = add_vec3(*r, scale_vec3(k3r, h)), v4 = add_vec3(*v, scale_vec3(k3v, h));
	Vector3 k4r = v4, k4v = gravity_acceleration(set, jd + day, r4, NULL);
	Vector3 dr = add_vec3(add_vec3(k1r, scale_vec3(k2r, 2)), add_vec3(scale_vec3(k3r, 2), k4r));
	Vector3 dv = add_vec3(add_vec3(k1v, scale_vec3(k2v, 2)), add_vec3(scale_vec3(k3v, 2), k4v));
	*r = add_vec3(*r, scale_vec3(dr, h / 6));
	*v = add_vec3(*v, scale_vec3(dv, h / 6));
}

// Integrates over duration seconds; the step follows the distance to the planet and to the nearest moon.
static int integrate_leg(const MoonSet *set, double jd, double duration, Vector3 *r, Vector3 *v) {
	double elapsed = 0;
	for(int steps = 0; elapsed < duration * (1 - 1e-9) && steps < MAX_INTEGRATION_STEPS; steps++) {
		double nearest;
		gravity_acceleration(set, jd + elapsed / 86400, *r, &nearest);
		double scale = fmin(mag_vec3(*r), nearest);
		double h = 0.01 * scale / fmax(mag_vec3(*v), 1);
		h = fmin(fmax(h, 0.5), duration - elapsed);
		rk4_step(set, jd + elapsed / 86400, h, r, v);
		elapsed += h;
		if(!isfinite(r->x + r->y + r->z + v->x + v->y + v->z)) return 0;
	}
	return elapsed >= duration * (1 - 1e-9);
}

static int integrate_moon_gravity(const MoonSet *set, const PoweredFlybyResult *flyby, double flyby_jd, Vector3 *velocity_error) {
	Vector3 r = flyby->incoming_soi_state.r, v = flyby->incoming_soi_state.v;
	if(!integrate_leg(set, flyby_jd - flyby->incoming_soi_time / 86400, flyby->incoming_soi_time, &r, &v)) return 0;
	v = add_vec3(v, flyby->burn_delta_v_vector);
	if(!integrate_leg(set, flyby_jd, flyby->outgoing_soi_time, &r, &v)) return 0;
	*velocity_error = subtract_vec3(v, flyby->outgoing_soi_state.v);
	return 1;
}

int analyze_moon_flyby(Body *planet, const PoweredFlybyResult *flyby, double flyby_jd, int integrate_gravity, MoonFlybyEffect *effect) {
	*effect = (MoonFlybyEffect){.closest_moon_distance = HUGE_VAL};
	if(planet == NULL || planet->system == NULL || planet->system->num_bodies <= 0 || !(planet->mu > 0) ||
		flyby == NULL || !(flyby->soi_radius > 0)) return 0;
	HyperbolaLeg legs[2];
	init_leg(&legs[0], flyby, planet->mu, 0);
	init_leg(&legs[1], flyby, planet->mu, 1);
	if(!(legs[0].excess_speed > 0) || !(legs[1].excess_speed > 0)) return 0;

	MoonSet influence = {.planet = planet};
	for(int i = 0; i < planet->system->num_bodies; i++) {
		Body *moon = planet->system->bodies[i];
		MoonFrame frame;
		if(moon == NULL || !(moon->radius > 0) || !moon_frame(planet, moon, flyby_jd, &frame)) continue;
		effect->has_moons = 1;
		double moon_speed = frame.speed;
		// An elliptical orbit sweeps the shell from periapsis to apoapsis, so centre it on the semi-major axis.
		int elliptical = moon->orbit.a > 0 && moon->orbit.e < 1;
		double moon_radius = elliptical ? moon->orbit.a : frame.radius;
		double ecc_margin = elliptical ? moon->orbit.a * moon->orbit.e : 0;
		double clear = moon_clear_radius(moon);
		double soi = moon_radius * pow(fmax(moon->mu, 0) / planet->mu, 0.4);
		double influence_distance = 2 * soi + clear;
		// Inside the moon's sphere of influence the planet-centred flyby model no longer describes the trajectory.
		double perturb_distance = soi + clear;
		for(int leg = 0; leg < 2; leg++) {
			Vector3 normal = frame.normal;
			// A coarse scan rules out most legs before the fine one that is needed to catch surface hits.
			double coarse = scan_leg(planet, moon, &legs[leg], flyby_jd, perturb_distance + ecc_margin, perturb_distance / 3, moon_radius, moon_speed,
				normal, perturb_distance + 0.05 * moon_radius, NULL);
			double hit = coarse > 1.2 * perturb_distance ? HUGE_VAL : scan_leg(planet, moon, &legs[leg], flyby_jd, perturb_distance + ecc_margin, clear / 2, moon_radius, moon_speed,
				normal, perturb_distance + 0.05 * moon_radius, NULL);
			if(hit < clear) {
				effect->hits_moon = 1;
				effect->moon_hit = moon;
			}
			if(hit < perturb_distance && moon->mu > 0) effect->perturbing_moon = moon;
			double near = integrate_gravity && moon->mu > 0 && influence_distance > clear ?
				scan_leg(planet, moon, &legs[leg], flyby_jd, influence_distance + ecc_margin, influence_distance / 4,
					moon_radius, moon_speed, normal, influence_distance + 0.05 * moon_radius, NULL) : HUGE_VAL;
			double closest = fmin(hit, near);
			if(closest < effect->closest_moon_distance) effect->closest_moon_distance = closest;
			if(near < influence_distance && influence.count < MAX_TRACKED_MOONS) {
				int known = 0;
				for(int j = 0; j < influence.count; j++) known |= influence.moons[j] == moon;
				if(!known) influence.moons[influence.count++] = moon;
			}
		}
	}
	if(!effect->hits_moon && influence.count > 0)
		effect->integrated = integrate_moon_gravity(&influence, flyby, flyby_jd, &effect->velocity_error);
	return effect->has_moons;
}

// Moon assists ---------------------------------------------------------------------------------------------

#define ASSIST_AIM_DIRECTIONS 8
#define ASSIST_ROTATION_DIRECTIONS 16
#define ASSIST_MIN_TIME 600.0
#define ASSIST_MIN_SAVING 5.0
#define ASSIST_MIN_SAVING_FRACTION 0.05

typedef struct {
	Body *planet;
	double arrival_radius;
	ArrivalCost arrival_cost;
} AssistGoal;

// Cost left after the moon encounter has left the spacecraft in this planet-centred state.
static double assist_residual_cost(const AssistGoal *goal, Vector3 r, Vector3 v) {
	double mu = goal->planet->mu;
	double energy = 0.5 * dot_vec3(v, v) - mu / mag_vec3(r);
	if(energy > 0 && dot_vec3(r, v) >= 0) return HUGE_VAL;
	// A bound orbit after the moon is costed as a capture from rest.
	return goal->arrival_cost(goal->planet, goal->arrival_radius, energy > 0 ? sqrt(2 * energy) : 0);
}

static void perpendicular_basis(Vector3 direction, Vector3 *first, Vector3 *second) {
	Vector3 axis = fabs(direction.z) < 0.9 ? vec3(0, 0, 1) : vec3(1, 0, 0);
	*first = norm_vec3(cross_vec3(direction, axis));
	*second = cross_vec3(direction, *first);
}

// An arriving spacecraft aims at a moon with a small manoeuvre (distance / time left), and the moon turns its velocity
// by up to the angle allowed by the moon's surface; the cheapest outcome that beats plain_cost is returned.
static int search_moon_assist(const AssistGoal *goal, Vector3 incoming_excess, const double *periapses, int num_periapses,
		double jd, double plain_cost, MoonAssist *assist) {
	Body *planet = goal->planet;
	double mu = planet->mu, speed = mag_vec3(incoming_excess);
	if(planet->system == NULL || planet->system->num_bodies <= 0 || planet->orbit.cb == NULL ||
		!(mu > 0) || !(speed > 0) || !(plain_cost > 0)) return 0;
	double soi = fabs(planet->orbit.a) * pow(mu / planet->orbit.cb->mu, 0.4);
	Vector3 direction = scale_vec3(incoming_excess, 1 / speed), aim_first, aim_second;
	perpendicular_basis(direction, &aim_first, &aim_second);
	// An assist must save a clear margin, since it also needs a different trajectory.
	double best_total = plain_cost - fmax(ASSIST_MIN_SAVING, ASSIST_MIN_SAVING_FRACTION * plain_cost);
	if(!(best_total > 0)) return 0;
	int found = 0;
	for(int p = 0; p < num_periapses; p++) {
		double eccentricity = 1 + periapses[p] * speed * speed / mu;
		double sine = 1 / eccentricity, cosine = sqrt(1 - sine * sine);
		HyperbolaLeg leg = {.mu = mu, .excess_speed = speed, .periapsis = periapses[p], .eccentricity = eccentricity,
			.semi_major_axis = mu / (speed * speed)};
		leg.mean_motion = sqrt(mu / pow(leg.semi_major_axis, 3));
		double soi_cosine = (soi / leg.semi_major_axis + 1) / eccentricity;
		if(soi_cosine <= 1) continue;
		leg.soi_time = leg_time(&leg, acosh(soi_cosine));
		for(int k = 0; k < ASSIST_AIM_DIRECTIONS; k++) {
			double aim_angle = 2 * M_PI * k / ASSIST_AIM_DIRECTIONS;
			Vector3 side = add_vec3(scale_vec3(aim_first, cos(aim_angle)), scale_vec3(aim_second, sin(aim_angle)));
			leg.periapsis_direction = add_vec3(scale_vec3(direction, sine), scale_vec3(side, cosine));
			leg.tangent = subtract_vec3(scale_vec3(direction, cosine), scale_vec3(side, sine));
			for(int i = 0; i < planet->system->num_bodies; i++) {
				Body *moon = planet->system->bodies[i];
				MoonFrame frame;
				if(moon == NULL || !(moon->radius > 0) || !(moon->mu > 0) || !moon_frame(planet, moon, jd, &frame)) continue;
				int elliptical = moon->orbit.a > 0 && moon->orbit.e < 1;
				double moon_radius = elliptical ? moon->orbit.a : frame.radius;
				double ecc_margin = elliptical ? moon->orbit.a * moon->orbit.e : 0;
				double clear = moon_clear_radius(moon);
				double moon_soi = moon_radius * pow(moon->mu / mu, 0.4);
				double reach = fmin(best_total * leg.soi_time, 3 * moon_soi + clear);
				if(reach <= clear) continue;
				ScanHit hit;
				scan_leg(planet, moon, &leg, jd, reach + ecc_margin, reach / 4, moon_radius, frame.speed, frame.normal,
					reach + 0.05 * moon_radius, &hit);
				if(!(hit.distance < reach)) continue;
				double aim_cost = hit.distance / fmax(leg.soi_time + hit.time, ASSIST_MIN_TIME);
				if(aim_cost >= best_total) continue;
				Vector3 relative_velocity = subtract_vec3(hit.velocity, hit.moon_velocity);
				double relative_speed = mag_vec3(relative_velocity);
				if(!(relative_speed > 0)) continue;
				double moon_eccentricity = 1 + clear * relative_speed * relative_speed / moon->mu;
				double maximum_turn = 2 * asin(1 / moon_eccentricity);
				Vector3 along = scale_vec3(relative_velocity, 1 / relative_speed), turn_first, turn_second;
				perpendicular_basis(along, &turn_first, &turn_second);
				for(int fraction = 0; fraction < 2; fraction++) {
					double turn = maximum_turn * (fraction == 0 ? 1 : 0.5);
					for(int j = 0; j < ASSIST_ROTATION_DIRECTIONS; j++) {
						double turn_angle = 2 * M_PI * j / ASSIST_ROTATION_DIRECTIONS;
						Vector3 turn_side = add_vec3(scale_vec3(turn_first, cos(turn_angle)), scale_vec3(turn_second, sin(turn_angle)));
						Vector3 turned = scale_vec3(add_vec3(scale_vec3(along, cos(turn)), scale_vec3(turn_side, sin(turn))), relative_speed);
						double residual = assist_residual_cost(goal, hit.position, add_vec3(hit.moon_velocity, turned));
						if(residual + aim_cost < best_total) {
							best_total = residual + aim_cost;
							double used_eccentricity = fraction == 0 ? moon_eccentricity : 1 / sin(turn / 2);
							*assist = (MoonAssist){moon, best_total, aim_cost, hit.position, hit.velocity, hit.moon_velocity,
								turned, hit.time, (used_eccentricity - 1) * moon->mu / (relative_speed * relative_speed)};
							found = 1;
						}
					}
				}
			}
		}
	}
	return found;
}

int moon_assist_arrival(Body *planet, Vector3 incoming_excess, double arrival_radius, ArrivalCost cost, double plain_dv,
		double arrival_jd, MoonAssist *assist) {
	AssistGoal goal = {.planet = planet, .arrival_radius = arrival_radius, .arrival_cost = cost};
	double periapses[] = {arrival_radius, 2 * arrival_radius, 4 * arrival_radius};
	return search_moon_assist(&goal, incoming_excess, periapses, 3, arrival_jd, plain_dv, assist);
}

int moon_propagate_outbound(Body *planet, double jd, double max_seconds, double soi_radius, Vector3 *r, Vector3 *v,
		double *elapsed_out, Body **hit) {
	*hit = NULL;
	*elapsed_out = 0;
	if(planet == NULL || planet->system == NULL || !(planet->mu > 0) || !(soi_radius > 0)) return 0;
	// KSP flies patched conics: moons pull only inside their own sphere of influence. The path is therefore two-body
	// around the planet, and entering a moon's sphere of influence (or surface clearance) counts as a hit.
	MoonSet pure = {.planet = planet}, set = {.planet = planet};
	double moon_soi[MAX_TRACKED_MOONS];
	for(int i = 0; i < planet->system->num_bodies && set.count < MAX_TRACKED_MOONS; i++) {
		Body *moon = planet->system->bodies[i];
		OSV state;
		if(moon != NULL && moon != planet && moon->orbit.cb == planet && moon->radius > 0 && moon->mu > 0 &&
			moon_state_at(planet, moon, jd, &state)) {
			moon_soi[set.count] = fmax(fabs(moon->orbit.a) * pow(moon->mu / planet->mu, 0.4), moon_clear_radius(moon));
			set.moons[set.count++] = moon;
		}
	}
	double elapsed = 0;
	for(int steps = 0; steps < MAX_INTEGRATION_STEPS && elapsed < max_seconds; steps++) {
		double now = jd + elapsed / 86400;
		double radius = mag_vec3(*r);
		if(radius >= soi_radius) {
			*elapsed_out = elapsed;
			return 1;
		}
		if(radius < planet->radius + planet->atmo_alt) {
			*hit = planet;
			*elapsed_out = elapsed;
			return 1;
		}
		double nearest = HUGE_VAL;
		for(int i = 0; i < set.count; i++) {
			OSV moon;
			if(!moon_state_at(planet, set.moons[i], now, &moon)) continue;
			double distance = mag_vec3(subtract_vec3(moon.r, *r));
			if(distance < moon_soi[i]) {
				*hit = set.moons[i];
				*elapsed_out = elapsed;
				return 1;
			}
			nearest = fmin(nearest, distance - moon_soi[i]);
		}
		double h = 0.005 * radius / fmax(mag_vec3(*v), 1);
		if(nearest < HUGE_VAL) h = fmin(h, 0.05 * fmax(nearest, 0) / fmax(mag_vec3(*v), 1) + 1);
		h = fmin(fmax(h, 0.5), 900);
		Vector3 r_before = *r, v_before = *v;
		rk4_step(&pure, now, h, r, v);
		if(!isfinite(r->x + r->y + r->z + v->x + v->y + v->z)) return 0;
		if(mag_vec3(*r) >= soi_radius) {
			// The patch changes exactly at the sphere of influence, so locate the crossing within the step.
			double low = 0, high = h;
			for(int i = 0; i < 40; i++) {
				double mid = (low + high) / 2;
				Vector3 r_mid = r_before, v_mid = v_before;
				rk4_step(&pure, now, mid, &r_mid, &v_mid);
				if(mag_vec3(r_mid) >= soi_radius) high = mid; else low = mid;
			}
			*r = r_before;
			*v = v_before;
			rk4_step(&pure, now, high, r, v);
			elapsed += high;
			*elapsed_out = elapsed;
			return 1;
		}
		elapsed += h;
	}
	*elapsed_out = elapsed;
	return mag_vec3(*r) >= soi_radius;
}
