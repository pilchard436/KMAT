#ifndef KMAT_MOON_FLYBY_H
#define KMAT_MOON_FLYBY_H

#include "orbitlib.h"

typedef struct MoonFlybyEffect {
	int has_moons;
	int hits_moon;
	Body *moon_hit;
	Body *perturbing_moon;        // the flyby passes inside this moon's sphere of influence
	int integrated;               // moon gravity was integrated because a moon passed close
	Vector3 velocity_error;       // outgoing excess velocity change caused by the moons
	double closest_moon_distance; // planned trajectory to the nearest moon centre in the scanned windows
} MoonFlybyEffect;

/**
 * @brief Checks a powered flyby of a planet against the planet's moons at the flyby epoch.
 * @param flyby_jd epoch of the flyby periapsis (Julian date)
 * @param integrate_gravity also integrate moon gravity for close passes (slow; not for the search)
 * @return 0 when the planet has no usable moons
 */
int analyze_moon_flyby(Body *planet, const PoweredFlybyResult *flyby, double flyby_jd, int integrate_gravity, MoonFlybyEffect *effect);

typedef struct MoonAssist {
	Body *moon;
	double total_dv; // burn or capture cost left after the moon, including the aiming manoeuvre
	double aim_dv;
} MoonAssist;

typedef double (*ArrivalCost)(Body *body, double periapsis_radius, double excess_speed);

/**
 * @brief Cheapest way a moon of the planet can lower the capture or circularisation cost on arrival.
 * Aiming costs distance / time left; the moon then turns the planet-relative velocity within the limit set by its surface.
 * @return 1 and the assist when it costs less than plain_dv (and saves a clear margin)
 */
int moon_assist_arrival(Body *planet, Vector3 incoming_excess, double arrival_radius, ArrivalCost cost, double plain_dv,
		double arrival_jd, MoonAssist *assist);

/** @brief Position and velocity of a moon relative to its planet. */
int moon_state_at(Body *planet, Body *moon, double jd, OSV *state);

#endif
