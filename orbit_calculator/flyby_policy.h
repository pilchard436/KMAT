#ifndef KMAT_FLYBY_POLICY_H
#define KMAT_FLYBY_POLICY_H

#include "itin_tool.h"

// Smallest periapsis radius a powered flyby may use: surface, atmosphere and 10 km, or 10% of the body radius
// (inside the body) while unrestricted. Unrestricted mode is per thread; it returns the previous setting.
double flyby_minimum_periapsis(Body *body);
int set_flyby_unrestricted_periapsis(int enable);
// Metres by which the unpowered periapsis of a flyby step lies below the minimum safe radius (surface, atmosphere
// and 10 km); 0 if it can be flown as a gravity assist or the step is not a flyby. A positive value means the
// unpowered trajectory would enter the planet or its atmosphere and the route only works with the flyby burn.
double flyby_periapsis_shortfall(struct ItinStep *step);
// 1 if the flown periapsis of a flyby step is below the minimum safe radius (surface, atmosphere and 10 km): no burn can fix it.
int flyby_below_min_safe(struct ItinStep *step);
// 1 if a powered flyby with these velocities can be flown at or above the minimum periapsis (cached).
int flyby_is_viable(Vector3 v_arr, Vector3 v_dep, Vector3 v_body, Body *body);
// Shortest leg the search and optimizer accept: 10 days, or an eighth of the shortest body period of
// a system with faster orbits (moons), so short transfers are not rejected. NULL gives the 10 days.
double minimum_leg_days(const CelestSystem *system);
int count_flybys_below_min_safe(struct ItinStep *step_in_route);
// Number of flybys in the route with a shortfall, and the largest shortfall in metres (largest may be NULL).
int count_flybys_needing_burn(struct ItinStep *step_in_route, double *largest_shortfall);

#endif
