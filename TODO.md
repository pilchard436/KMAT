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





