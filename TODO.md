# TODO

## Current Context

- Powered flybys use patched-conic, two-body hyperbola propagation from SOI entry to periapsis and out to SOI exit. Route matching uses Lambert legs between SOI boundary states and iterates flyby timing and geometry.
- Powered burns are applied at periapsis. The radial component is zero by construction; tangential and normal components are supported.
- Preserve user-configured date and delta-v limits as the intentional route filters. Internal search approximations and iteration limits should not silently exclude otherwise feasible routes.

## Parking Orbit

- [ ] Add configuration for all six classical orbital elements: semi-major axis, eccentricity, inclination, RAAN, argument of periapsis, and true anomaly.
- [ ] Add a separate parking-orbit editor window that can be opened from the main page.
- [ ] Add a figure that updates to visualize the orbit as its elements change.

## Powered-Flyby Search Coverage

- [ ] Review the `fabs(diff_vinf) < 1 m/s` flyby-generation gate in `orbit_calculator/itin_tool.c`. Generate powered-flyby candidates without requiring near-equal incoming and outgoing excess speeds.
- [ ] Review the hardcoded 10-day minimum transfer-leg duration in `orbit_calculator/powered_flyby_optimizer.c`. Do not let it act as an undocumented user constraint.
- [ ] Review the top-100 powered-route refinement limit. Ensure candidates outside the refinement set are not silently lost or misrepresented as fully SOI-corrected.
- [ ] Review SOI periapsis and sampling bounds in `external/orbitlib/src/transfer.c`, including the 10 km clearance, 95%-of-SOI periapsis ceiling, 64-point search, and refinement limits. Avoid rejecting feasible geometry because of a coarse or arbitrary bound.
- [ ] Measure and improve convergence-related route rejections in the iterative SOI/Lambert match. Distinguish numerical nonconvergence from physical infeasibility.
- [ ] Add categorized route rejection counts for SOI geometry, convergence, configured date/time limits, departure delta-v, arrival delta-v, and total delta-v.
- [ ] Use the full-profile UI-free search to quantify which otherwise feasible routes each internal cutoff excludes.



The limitation is the route search: find_viable_flybys() only generates candidates when the incoming and outgoing excess-speed magnitudes differ by less than 1 m/s. So KMAT can model an unequal-speed powered flyby, but the main search does not seed routes that require a substantial 
𝑉
∞
V 
∞
​
  speed change.

Don’t search for speed difference itself. Search over encounter dates, and at each date evaluate whether a powered flyby can connect the incoming and outgoing 
V
∞
V 
∞
​
  vectors.

In KMAT’s find_viable_flybys(), for each sampled Lambert departure date:

Compute incoming 
V
∞
=
v
arr
−
v
body
V 
∞
​
 =v 
arr
​
 −v 
body
​
  and outgoing 
V
∞
=
v
Lambert
,
0
−
v
body
V 
∞
​
 =v 
Lambert,0
​
 −v 
body
​
  at the current flyby body.
Pass both vectors to calculate_powered_flyby(). It checks flyby geometry and SOI feasibility, and computes the required periapsis burn and radius. Don’t reject a candidate just because the magnitudes differ by more than 1 m/s.
Retain several feasible dates across the search window, then rank/refine them using full mission Δv: departure burn + all powered-flyby burns + arrival/DSM burns.
A speed-changing flyby is worth keeping only if its periapsis cost leads to a better feasible total mission objective. Validate the search with a small case where the incoming/outgoing 
V
∞
V 
∞
​
  magnitudes differ by more than 1 m/s and compare its result to a coarse date grid.