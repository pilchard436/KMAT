#ifndef KMAT_MOON_FLYBY_H
#define KMAT_MOON_FLYBY_H

#include "orbitlib.h"

typedef struct MoonFlybyEffect {
	int has_moons;
	int hits_moon;
	Body *moon_hit;
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

/** @brief Position and velocity of a moon relative to its planet. */
int moon_state_at(Body *planet, Body *moon, double jd, OSV *state);

#endif
