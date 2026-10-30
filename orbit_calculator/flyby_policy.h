#ifndef KMAT_FLYBY_POLICY_H
#define KMAT_FLYBY_POLICY_H

#include "itin_tool.h"
#include "moon_flyby.h"

// Smallest periapsis radius a powered flyby may use: surface, atmosphere and 10 km.
double flyby_minimum_periapsis(Body *body);
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
// Powered flyby that checks the planet's moons at the flyby epoch when moon-aware flybys are enabled: 0 if the
// flyby hits a moon and no moon-free alternative exists.
int calculate_flyby_with_moons(Vector3 v_arr, Vector3 v_dep, Vector3 v_body, Body *body,
		double minimum_periapsis, double flyby_jd, PoweredFlybyResult *result);
// Cheapest periapsis radius and side that clears every moon at the flyby epoch (cached); 0 if none does.
// result, radius and side may be NULL. The side is the one passed to calculate_powered_flyby_at_periapsis.
int plan_flyby_around_moons(Vector3 v_arr, Vector3 v_dep, Vector3 v_body, Body *body, double minimum_periapsis,
		double jd, PoweredFlybyResult *result, double *radius, int *side);
// 1 if moon-aware flybys are enabled and this flyby would hit a moon or pass inside its sphere of influence.
int flyby_blocked_by_moon(Body *body, const PoweredFlybyResult *flyby, double flyby_jd);
// Moon the flown flyby of this step would hit, or NULL.
Body * flyby_moon_hit(struct ItinStep *step);
// Moon assists lower the capture or circularisation cost on arrival (see moon_flyby.h). They are applied only
// while explicitly enabled; relaxing the flyby periapsis limit in the planner does not enable them. Returns previous.
int set_moon_assist_enabled(int enable);
int moon_assist_is_active(void);
int set_moon_aware_flybys_enabled(int enable);
int moon_aware_flybys_is_active(void);
double arrival_dv_with_assist(Body *body, Vector3 incoming_excess, double arrival_radius, int circularize, double plain_dv,
		double jd, MoonAssist *assist);
// Shortest leg the search and optimizer accept: 10 days, or an eighth of the shortest body period of
// a system with faster orbits (moons), so short transfers are not rejected. NULL gives the 10 days.
double minimum_leg_days(const CelestSystem *system);
/* Most flybys a route of a target search may contain; deeper nodes only search legs to the target. Every flyby
   multiplies the route tree by the number of encounters in the remaining window, so routes with more flybys than
   this cost most of a long-duration search while contributing little. Setters apply to searches started afterwards;
   values below 1 are ignored, values above MAX_FLYBYS_PER_ROUTE are capped, and the previous value is returned. */
#define MAX_FLYBYS_PER_ROUTE 5
int set_max_flybys_per_route(int flybys);
int get_max_flybys_per_route(void);
// Number of flybys flown before `step`, i.e. the number of legs leading to it minus the departure.
int route_flyby_count(const struct ItinStep *step);
/* Longest leg the target search considers between two different bodies when the leg is not the final one: the
   larger of LEG_HORIZON_SYNODIC_PERIODS synodic periods of the pair (every encounter geometry recurs within one) and
   LEG_HORIZON_HOHMANN_DURATIONS Hohmann times from the current radius (the first-leg window). Longer windows mainly
   add encounters on later revolutions whose routes repeat earlier ones with a worse arrival. HUGE_VAL when the
   bodies are the same (resonant legs are bounded by the revolution count) or an orbit is unknown. */
#define LEG_HORIZON_SYNODIC_PERIODS 1.5
#define LEG_HORIZON_HOHMANN_DURATIONS 3.0
double maximum_intermediate_leg_days(double from_radius, const Body *from, const Body *to, const CelestSystem *system);
// 1 if moon-aware flybys are enabled and any flyby of the route cannot avoid a moon.
int route_blocked_by_moons(struct ItinStep *step_in_route);
int count_flybys_below_min_safe(struct ItinStep *step_in_route);
// Number of flybys in the route with a shortfall, and the largest shortfall in metres (largest may be NULL).
int count_flybys_needing_burn(struct ItinStep *step_in_route, double *largest_shortfall);

#endif
