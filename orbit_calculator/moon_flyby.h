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
	// Planet-centred geometry of the encounter, for drawing: where and when the aimed arrival leg meets the moon and how the
	// moon turns the craft (velocities are relative to the planet unless stated).
	Vector3 hit_position, hit_velocity, moon_velocity;
	Vector3 turned_velocity;    // relative to the moon, after the turn
	double hit_seconds;         // from the arrival date (the periapsis of the aimed leg)
	double moon_periapsis;      // radius of the hyperbola around the moon
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

/**
 * @brief Integrates a planet-centred state outward until it leaves soi_radius, as KSP's patched conics do: two-body
 * around the planet, with moons only mattering as a hit when the path enters their sphere of influence.
 * @param r, v state at jd, replaced by the state where the integration stopped
 * @param elapsed_out seconds integrated
 * @param hit set to a moon (or the planet) when the path comes within its surface clearance; integration stops there
 * @return 1 when the integration finished (left the sphere of influence or hit something)
 */
int moon_propagate_outbound(Body *planet, double jd, double max_seconds, double soi_radius, Vector3 *r, Vector3 *v,
	double *elapsed_out, Body **hit);

#endif
