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

// Closest distance from the planned leg to a moon, scanning only while the leg is inside the moon's orbital shell widened by margin.
static double scan_leg(Body *planet, Body *moon, const HyperbolaLeg *leg, double flyby_jd,
		double margin, double step_distance, double moon_radius, double moon_speed, Vector3 plane_normal, double plane_clearance) {
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
		Vector3 relative = subtract_vec3(leg_position(leg, leg->outgoing ? h : -h), state.r);
		double distance = mag_vec3(relative);
		if(distance < best) best = distance;
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
		double soi = integrate_gravity ? moon_radius * pow(fmax(moon->mu, 0) / planet->mu, 0.4) : 0;
		double influence_distance = 2 * soi + clear;
		for(int leg = 0; leg < 2; leg++) {
			Vector3 normal = frame.normal;
			double hit = scan_leg(planet, moon, &legs[leg], flyby_jd, clear + ecc_margin, clear / 2, moon_radius, moon_speed,
				normal, clear + 0.05 * moon_radius);
			if(hit < clear) {
				effect->hits_moon = 1;
				effect->moon_hit = moon;
			}
			double near = integrate_gravity && moon->mu > 0 && influence_distance > clear ?
				scan_leg(planet, moon, &legs[leg], flyby_jd, influence_distance + ecc_margin, influence_distance / 4,
					moon_radius, moon_speed, normal, influence_distance + 0.05 * moon_radius) : HUGE_VAL;
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
