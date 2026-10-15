# TODO

## Current Context

- Powered flybys use patched-conic, two-body hyperbola propagation from SOI entry through periapsis to SOI exit. Lambert legs between SOI boundaries iterate timing and geometry.
- Powered burns occur at periapsis; tangential and normal components are supported, with zero radial component.
- User date and delta-v limits remain physical mission constraints. Search budgets limit exploration and must be reported separately from infeasibility.

## Speed-Changing Powered-Flyby Branch

- [x] Generate powered candidates independently of the near-equal excess-speed root search.
- [x] Retain three distinct estimated candidates per date bucket, falling back when cheaper candidates fail feasibility.
- [x] Rank feasible bucket winners by calculated periapsis burn, retaining distinct dates with a bounded expansion budget.
- [x] Bound timing-refinement objective evaluations and allocate extra starts to competitive routes and distinct body sequences.
- [x] Fix regression traversal to scan until a speed-changing encounter is found, using the actual outbound route leg.
- [x] Add fallback-selection and pre-optimization speed-changing seed coverage checks.
- [x] Add an opt-in rejected-route audit with bounded reservoir sampling and timing repair. See `00_Testing/rejection_audit.md`; this measures optimizer-stage losses, not seed-budget or DSM coverage.
- [x] Run the full-year profile with route-constraint and corrected-objective checks; the budgeted search completed in 61.7 seconds locally. Baseline HEAD was stopped at 166 seconds before any departure completed. This establishes practical runtime on this fixture, not exhaustive coverage.
- [x] Repair a bounded, stratified sample of initially rejected routes and let recovered routes compete for refinement by repaired full-mission cost.
- [x] Give 32 sampled cutoff-excluded routes extra refinement slots to challenge the 500-route cutoff.
- [x] Write corrected routes as a full chain with `v_dep` on every node; the old suffix-only write-back left stale velocities and the final filter pruned the routes.
- [ ] One-DSM-per-leg exploration, starting with a controlled case comparing total mission delta-v against no DSM.

### Current Search Budgets

- Four date buckets, three shortlist candidates per bucket; at most twelve full powered-flyby feasibility evaluations per encounter/next-body search.
- At most 512 powered seeds per departure date, normally one per encounter/next-body search, with a second from another bucket while an eight-extra-branches budget remains. Both budgets reset independently for each departure date. Once 512 seeds are added, subsequent encounters keep the existing near-unpowered root search and skip powered sampling.
- Bucket sampling retains the existing two subdivisions per conjunction/opposition interval. This is finite sampling, not exhaustive date coverage.
- At most 500 routes receive timing refinement. Each receives at most 96 objective evaluations, excluding its initial SOI evaluation.
- Up to 200 routes receive three timing starts if their initial objective is within 10% of the best selected initial objective or they introduce a different body sequence. Other routes use one start; all share the same per-route evaluation cap.
- Up to 64 initially rejected routes (one reservoir slot per departure-date bin and body-sequence hash) get at most 144 timing-repair evaluations each, moving only their own dates. Recovered routes rank with the 500 by repaired cost; user limits are unchanged.
- 32 routes sampled from those excluded by the cutoff are refined beyond the 500.
- Logs report seed feasibility evaluations, added seeds, extra branches, skipped feasible bucket alternatives, encounters that skip powered sampling after budget exhaustion, objective evaluations, exhausted refinement budgets, and scoring/refinement wall time. Existing optional profiling reports Lambert calls.
- These budgets bound additional branching and per-route refinement, not total recursive search runtime. Full feasibility scoring still visits all generated complete candidates.

## Parking Orbit

- [ ] Add all six classical elements: semi-major axis, eccentricity, inclination, RAAN, argument of periapsis, and true anomaly.
- [ ] Add a separate parking-orbit editor accessible from the main page.
- [ ] Add a figure that updates as the elements change.

## Remaining Powered-Flyby Search Coverage

- [ ] Review the undocumented 10-day minimum leg duration in search and SOI refinement.
- [ ] Ensure routes outside the 500-route timing-refinement set are not misrepresented as SOI-corrected. Increasing a cap does not resolve this distinction.
- [ ] Review the 10 km clearance, 95%-of-SOI periapsis ceiling, 64-point radius search, and refinement limits in external/orbitlib/src/transfer.c.
- [ ] Distinguish SOI/Lambert nonconvergence from physical infeasibility and improve convergence-related route rejection.
- [ ] Add rejection counts for SOI geometry, convergence, configured date/time limits, departure delta-v, arrival delta-v, and total delta-v.
- [ ] Quantify coverage lost to each sampling/budget cutoff using the UI-free profile and objective-grid comparisons.

## Integration Tests

- [ ] Replace the all-bodies fixtures in `KMAT --test` (tests 4 and 5 took about 197 s and 506 s) with realistic body sets; `KMATIntegrationTests` currently hits the 1500 s ctest timeout.
- [ ] Give each integration test an explicit time budget and fail on overrun instead of relying on the ctest default.
- [ ] Check result quality, not only constraint validity: best delta-v against a reference, and that several distinct near-optimal routes are returned.
- [ ] Add per-test timing output and a recorded baseline so slowdowns are caught.
- [ ] Cover the `.itin`/`.itins` file tests, which are not part of the 10 reported tests.
- [ ] Add a UI-free check for the porkchop group list (select/deselect all, toggle keeps scroll position).
