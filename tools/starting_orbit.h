#ifndef KMAT_STARTING_ORBIT_H
#define KMAT_STARTING_ORBIT_H

#include "orbitlib.h"

/* Elements are in the same inertial frame as the departure velocity (the celestial system's
 * reference frame), so inclination, RAAN and argument of periapsis are relative to that frame. */
typedef struct StartingOrbit {
	double semi_major_axis; /* m, measured from the body's center */
	double eccentricity;
	double inclination, raan, arg_periapsis, true_anomaly; /* rad */
} StartingOrbit;

/* A circular, zero-inclination orbit at the given radius from the body's center. */
StartingOrbit starting_orbit_circular(double radius);

/* Finite elements, e in [0, 1), and a periapsis that clears the surface and atmosphere. */
int starting_orbit_is_valid(const StartingOrbit *orbit, const Body *body);

Orbit starting_orbit_to_orbit(const StartingOrbit *orbit, Body *body);
double starting_orbit_periapsis_radius(const StartingOrbit *orbit);
double starting_orbit_apoapsis_radius(const StartingOrbit *orbit);
double starting_orbit_period(const StartingOrbit *orbit, const Body *body);

/* State on the orbit at a true anomaly. */
int starting_orbit_state_at(const StartingOrbit *orbit, Body *body, double true_anomaly, OSV *state);

/* Burn that leaves the orbit at the given true anomaly on the hyperbola with excess velocity v_inf.
 * Both flight directions are tried and the cheaper one kept; fails if the hyperbola would pass
 * below the surface or the asymptote lies along the local radius. */
int starting_orbit_departure_burn_at(const StartingOrbit *orbit, Body *body, double true_anomaly,
	Vector3 v_inf, double *burn_magnitude, Vector3 *burn_vector);

/* The same burn minimized over the whole orbit, assuming the spacecraft can wait for the best point. */
int starting_orbit_best_departure_burn(const StartingOrbit *orbit, Body *body, Vector3 v_inf,
	double *burn_magnitude, double *best_true_anomaly);

/* Departure orbit used by the search and the delta-v displays. It is a calculation setting, not stored
 * in itinerary files, and must be set before a search starts. NULL clears it. */
void starting_orbit_set_active(const StartingOrbit *orbit, Body *body);
int starting_orbit_get_active(StartingOrbit *orbit, Body **body);

/* Departure delta-v for an excess velocity: the active starting orbit if it belongs to this body,
 * otherwise the circular free-plane burn at periapsis_radius. A starting orbit that cannot reach the
 * required asymptote returns HUGE_VAL, so limits reject the route. Returns 0 only if the circular
 * fallback is undefined for this velocity. */
int starting_orbit_departure_dv(Body *body, Vector3 v_inf, double periapsis_radius, double *dv);

#endif
