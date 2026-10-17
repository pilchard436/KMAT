#ifndef KMAT_FLYBY_POLICY_H
#define KMAT_FLYBY_POLICY_H

#include "itin_tool.h"
#include "moon_flyby.h"

// Smallest periapsis radius a powered flyby may use: surface, atmosphere and 10 km, or 10% of the body radius
// (inside the body) while unrestricted. Unrestricted mode is per thread; it returns the previous setting.
double flyby_minimum_periapsis(Body *body);
int set_flyby_unrestricted_periapsis(int enable);
// Metres by which the unpowered periapsis of a flyby step lies below the minimum safe radius (surface, atmosphere
// and 10 km); 0 if it can be flown as a gravity assist or the step is not a flyby. A positive value means the
// unpowered trajectory would enter the planet or its atmosphere and the route only works with the flyby burn.
double flyby_periapsis_shortfall(struct ItinStep *step);
// 1 if the flown periapsis of a flyby step is below the minimum safe radius (surface, atmosphere and 10 km), or the
// flyby hits a moon of the body: no burn can fix it.
int flyby_below_min_safe(struct ItinStep *step);
// 1 if a powered flyby with these velocities at the given epoch can be flown at or above the minimum periapsis
// without hitting a moon of the body (cached).
int flyby_is_viable(Vector3 v_arr, Vector3 v_dep, Vector3 v_body, Body *body, double jd);
// Powered flyby that also checks the planet's moons at the flyby epoch: 0 if the flyby hits a moon (unless the
// periapsis limit is relaxed), and the burn includes the correction for moon gravity when a moon passes close
// (only while the periapsis limit is relaxed, i.e. in the planner; the search only checks for hits).
int calculate_flyby_with_moons(Vector3 v_arr, Vector3 v_dep, Vector3 v_body, Body *body,
		double minimum_periapsis, double flyby_jd, PoweredFlybyResult *result);
// Cheapest periapsis radius and side that clears every moon at the flyby epoch (cached); 0 if none does.
// result, radius and side may be NULL. The side is the one passed to calculate_powered_flyby_at_periapsis.
int plan_flyby_around_moons(Vector3 v_arr, Vector3 v_dep, Vector3 v_body, Body *body, double minimum_periapsis,
		double jd, PoweredFlybyResult *result, double *radius, int *side);
// 1 if this flyby would hit a moon or pass inside a moon's sphere of influence (so its moon-free model is invalid)
// and the periapsis limit is not relaxed.
int flyby_blocked_by_moon(Body *body, const PoweredFlybyResult *flyby, double flyby_jd);
// Moon the flown flyby of this step would hit, or NULL.
Body * flyby_moon_hit(struct ItinStep *step);
// Moon assists lower the capture or circularisation cost on arrival (see moon_flyby.h). The search nodes keep
// plain costs; the final route scoring always applies assists, other callers only while this is enabled or the
// periapsis limit is relaxed (planner). Returns the previous setting.
int set_moon_assist_enabled(int enable);
int moon_assist_is_active(void);
double arrival_dv_with_assist(Body *body, Vector3 incoming_excess, double arrival_radius, int circularize, double plain_dv,
		double jd, MoonAssist *assist);
// Shortest leg the search and optimizer accept: 10 days, or an eighth of the shortest body period of
// a system with faster orbits (moons), so short transfers are not rejected. NULL gives the 10 days.
double minimum_leg_days(const CelestSystem *system);
// 1 if any flyby of the route is blocked by a moon (see flyby_blocked_by_moon).
int route_blocked_by_moons(struct ItinStep *step_in_route);
int count_flybys_below_min_safe(struct ItinStep *step_in_route);
// Number of flybys in the route with a shortfall, and the largest shortfall in metres (largest may be NULL).
int count_flybys_needing_burn(struct ItinStep *step_in_route, double *largest_shortfall);

#endif
