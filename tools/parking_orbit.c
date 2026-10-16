#include "parking_orbit.h"

#include <float.h>
#include <math.h>

#define BURN_SAMPLES 120
#define BURN_REFINE_ITERATIONS 48
#define BURN_PARALLEL_LIMIT 1e-9

static ParkingOrbit active_orbit;
static Body *active_body;

static int is_finite_vector(Vector3 value) {
	return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}

ParkingOrbit parking_orbit_circular(double radius) {
	return (ParkingOrbit){.semi_major_axis = radius};
}

double parking_orbit_periapsis_radius(const ParkingOrbit *orbit) {
	return orbit->semi_major_axis * (1 - orbit->eccentricity);
}

double parking_orbit_apoapsis_radius(const ParkingOrbit *orbit) {
	return orbit->semi_major_axis * (1 + orbit->eccentricity);
}

double parking_orbit_period(const ParkingOrbit *orbit, const Body *body) {
	return 2 * M_PI * sqrt(orbit->semi_major_axis * orbit->semi_major_axis * orbit->semi_major_axis / body->mu);
}

int parking_orbit_is_valid(const ParkingOrbit *orbit, const Body *body) {
	if(orbit == NULL || body == NULL || !(body->mu > 0)) return 0;
	if(!isfinite(orbit->semi_major_axis) || !isfinite(orbit->eccentricity) ||
		!isfinite(orbit->inclination) || !isfinite(orbit->raan) ||
		!isfinite(orbit->arg_periapsis) || !isfinite(orbit->true_anomaly)) return 0;
	if(!(orbit->semi_major_axis > 0) || !(orbit->eccentricity >= 0 && orbit->eccentricity < 1)) return 0;
	return parking_orbit_periapsis_radius(orbit) > body->radius + body->atmo_alt;
}

Orbit parking_orbit_to_orbit(const ParkingOrbit *orbit, Body *body) {
	return constr_orbit_from_elements(orbit->semi_major_axis, orbit->eccentricity, orbit->inclination,
		orbit->raan, orbit->arg_periapsis, orbit->true_anomaly, body);
}

int parking_orbit_state_at(const ParkingOrbit *orbit, Body *body, double true_anomaly, OSV *state) {
	if(!parking_orbit_is_valid(orbit, body) || !isfinite(true_anomaly)) return 0;
	Orbit shape = parking_orbit_to_orbit(orbit, body);
	shape.ta = true_anomaly;
	*state = osv_from_orbit(shape);
	return is_finite_vector(state->r) && is_finite_vector(state->v);
}

/* The post-burn hyperbola through r with outgoing asymptote s_hat and speed v_inf is fixed by one
 * angle: delta, the angle from r to s_hat in the direction of motion. With x = sqrt(e^2 - 1) and
 * k = mu / v_inf^2, the orbit equation r = p / (1 + e cos(nu)) at nu = nu_inf - delta reduces to
 * k x^2 - r sin(delta) x - r (1 - cos(delta)) = 0, whose single positive root gives the state. */
static int burn_from_state(const Body *body, OSV parking, Vector3 s_hat, double v_inf,
		double *burn_magnitude, Vector3 *burn_vector) {
	double radius = mag_vec3(parking.r);
	if(!(radius > 0)) return 0;
	Vector3 r_hat = scale_vec3(parking.r, 1 / radius);
	Vector3 normal = cross_vec3(r_hat, s_hat);
	double sin_geometry = mag_vec3(normal);
	if(sin_geometry < BURN_PARALLEL_LIMIT) return 0;
	Vector3 h_hat = scale_vec3(normal, 1 / sin_geometry);
	double geometry_angle = atan2(sin_geometry, dot_vec3(r_hat, s_hat));
	double k = body->mu / (v_inf * v_inf);
	double minimum_radius = body->radius + body->atmo_alt;

	double best = DBL_MAX;
	Vector3 best_vector = vec3(0, 0, 0);
	for(int orientation = 0; orientation < 2; orientation++) {
		double delta = orientation == 0 ? geometry_angle : 2 * M_PI - geometry_angle;
		Vector3 orbit_normal = orientation == 0 ? h_hat : scale_vec3(h_hat, -1);
		double sin_delta = sin(delta), cos_delta = cos(delta);
		double x = (radius * sin_delta + sqrt(radius * radius * sin_delta * sin_delta +
			4 * k * radius * (1 - cos_delta))) / (2 * k);
		if(!(x > 0) || !isfinite(x)) continue;
		double eccentricity = sqrt(1 + x * x);
		double periapsis = k * x * x / (1 + eccentricity);
		double nu = acos(-1 / eccentricity) - delta;
		nu = atan2(sin(nu), cos(nu));
		// Before periapsis the hyperbola still has to get there without hitting the body.
		if(nu < 0 && periapsis < minimum_radius) continue;
		double angular_momentum = body->mu * x / v_inf;
		double radial_speed = v_inf * (cos_delta + sin_delta / x);
		double tangential_speed = angular_momentum / radius;
		Vector3 t_hat = cross_vec3(orbit_normal, r_hat);
		Vector3 velocity = add_vec3(scale_vec3(r_hat, radial_speed), scale_vec3(t_hat, tangential_speed));
		Vector3 burn = subtract_vec3(velocity, parking.v);
		double magnitude = mag_vec3(burn);
		if(isfinite(magnitude) && magnitude < best) {
			best = magnitude;
			best_vector = burn;
		}
	}
	if(best >= DBL_MAX) return 0;
	*burn_magnitude = best;
	if(burn_vector != NULL) *burn_vector = best_vector;
	return 1;
}

int parking_orbit_departure_burn_at(const ParkingOrbit *orbit, Body *body, double true_anomaly,
		Vector3 v_inf, double *burn_magnitude, Vector3 *burn_vector) {
	double v_inf_magnitude = mag_vec3(v_inf);
	if(!(v_inf_magnitude > 0) || !isfinite(v_inf_magnitude) || burn_magnitude == NULL) return 0;
	OSV parking;
	if(!parking_orbit_state_at(orbit, body, true_anomaly, &parking)) return 0;
	return burn_from_state(body, parking, scale_vec3(v_inf, 1 / v_inf_magnitude), v_inf_magnitude,
		burn_magnitude, burn_vector);
}

int parking_orbit_best_departure_burn(const ParkingOrbit *orbit, Body *body, Vector3 v_inf,
		double *burn_magnitude, double *best_true_anomaly) {
	double values[BURN_SAMPLES];
	double step = 2 * M_PI / BURN_SAMPLES;
	double best = DBL_MAX, best_anomaly = 0;
	for(int i = 0; i < BURN_SAMPLES; i++) {
		double burn;
		values[i] = parking_orbit_departure_burn_at(orbit, body, i * step, v_inf, &burn, NULL) ? burn : DBL_MAX;
		if(values[i] < best) { best = values[i]; best_anomaly = i * step; }
	}
	if(best >= DBL_MAX) return 0;

	// Each sampled local minimum is refined by golden-section search between its neighbours.
	for(int i = 0; i < BURN_SAMPLES; i++) {
		double before = values[(i + BURN_SAMPLES - 1) % BURN_SAMPLES], after = values[(i + 1) % BURN_SAMPLES];
		if(values[i] >= DBL_MAX || values[i] > before || values[i] > after) continue;
		double low = (i - 1) * step, high = (i + 1) * step;
		const double golden = 0.6180339887498949;
		double left = high - golden * (high - low), right = low + golden * (high - low);
		double left_value = DBL_MAX, right_value = DBL_MAX, burn;
		if(parking_orbit_departure_burn_at(orbit, body, left, v_inf, &burn, NULL)) left_value = burn;
		if(parking_orbit_departure_burn_at(orbit, body, right, v_inf, &burn, NULL)) right_value = burn;
		for(int iteration = 0; iteration < BURN_REFINE_ITERATIONS; iteration++) {
			if(left_value <= right_value) {
				high = right; right = left; right_value = left_value;
				left = high - golden * (high - low);
				left_value = parking_orbit_departure_burn_at(orbit, body, left, v_inf, &burn, NULL) ? burn : DBL_MAX;
			} else {
				low = left; left = right; left_value = right_value;
				right = low + golden * (high - low);
				right_value = parking_orbit_departure_burn_at(orbit, body, right, v_inf, &burn, NULL) ? burn : DBL_MAX;
			}
		}
		double candidate = left_value <= right_value ? left_value : right_value;
		double candidate_anomaly = left_value <= right_value ? left : right;
		if(candidate < best) { best = candidate; best_anomaly = candidate_anomaly; }
	}
	*burn_magnitude = best;
	if(best_true_anomaly != NULL) *best_true_anomaly = fmod(fmod(best_anomaly, 2 * M_PI) + 2 * M_PI, 2 * M_PI);
	return 1;
}

void parking_orbit_set_active(const ParkingOrbit *orbit, Body *body) {
	if(orbit == NULL || body == NULL) {
		active_body = NULL;
		return;
	}
	active_orbit = *orbit;
	active_body = body;
}

int parking_orbit_get_active(ParkingOrbit *orbit, Body **body) {
	if(active_body == NULL) return 0;
	if(orbit != NULL) *orbit = active_orbit;
	if(body != NULL) *body = active_body;
	return 1;
}

int parking_orbit_departure_dv(Body *body, Vector3 v_inf, double periapsis_radius, double *dv) {
	if(active_body != NULL && body == active_body) {
		double burn;
		*dv = parking_orbit_best_departure_burn(&active_orbit, body, v_inf, &burn, NULL) ? burn : HUGE_VAL;
		return 1;
	}
	DepartureBurn3D free_plane;
	if(calculate_free_plane_departure_burn(v_inf, body, periapsis_radius, &free_plane)) {
		*dv = free_plane.burn_magnitude;
		return 1;
	}
	return 0;
}
