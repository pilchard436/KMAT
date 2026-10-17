# TODO

## Current Context

- Powered flybys use patched-conic, two-body hyperbola propagation from SOI entry through periapsis to SOI exit. Lambert legs between SOI boundaries iterate timing and geometry.
- Powered burns occur at periapsis; tangential and normal components are supported, with zero radial component.
- User date and delta-v limits remain physical mission constraints. Search budgets limit exploration and must be reported separately from infeasibility.

### Current Search Budgets

- Four date buckets, three shortlist candidates per bucket; at most twelve full powered-flyby feasibility evaluations per encounter/next-body search.
- At most 512 powered seeds per departure date, normally one per encounter/next-body search, with a second from another bucket while an eight-extra-branches budget remains. Both budgets reset independently for each departure date. Once 512 seeds are added, subsequent encounters keep the existing near-unpowered root search and skip powered sampling.
- Bucket sampling retains the existing two subdivisions per conjunction/opposition interval. This is finite sampling, not exhaustive date coverage.
- At most 500 routes receive timing refinement. Each receives at most 96 objective evaluations, excluding its initial SOI evaluation.
- Up to 200 routes receive three timing starts if their initial objective is within 10% of the best selected initial objective or they introduce a different body sequence. Other routes use one start; all share the same per-route evaluation cap.
- Up to 64 initially rejected routes (one reservoir slot per departure-date bin and body-sequence hash) get at most 144 timing-repair evaluations each, moving only their own dates. Recovered routes rank with the 500 by repaired cost; user limits are unchanged.
- 32 routes sampled from those excluded by the cutoff are refined beyond the 500.
- With deep-space maneuvers allowed, the 50 best-ranked routes (plus up to 30 resonant ones) get at most 900 extra objective evaluations each; a limit of 100 or 200 gave no better route on the Earth-Venus-Jupiter fixture, and a leg keeps its DSM only if it saves at least 5 m/s with at least a 1 m/s maneuver.
- Logs report seed feasibility evaluations, added seeds, extra branches, skipped feasible bucket alternatives, encounters that skip powered sampling after budget exhaustion, objective evaluations, exhausted refinement budgets, and scoring/refinement wall time. Existing optional profiling reports Lambert calls.
- These budgets bound additional branching and per-route refinement, not total recursive search runtime. Full feasibility scoring still visits all generated complete candidates.

## Remaining Powered-Flyby Search Coverage

- [x] Review the undocumented 10-day minimum leg duration in search and SOI refinement.
- [x] Moon flybys: a flyby that hits a moon is re-planned at another periapsis radius and side; moon gravity is added as a velocity-error burn only when a moon passes within twice its sphere of influence; search both choices of periapsis side and radius around moons.
- [x] Review the 10 km clearance, 95%-of-SOI periapsis ceiling, 64-point radius search, and refinement limits in external/orbitlib/src/transfer.c.
- [x] When a search returns no routes, say which limit removed them (for example all routes exceeded the departure delta-v limit) instead of showing an empty result.
- [x] Quantify coverage lost to each sampling/budget cutoff using the UI-free profile and objective-grid comparisons.
- [ ] Multi-flyby routes are under-searched at the default limits (Earth-Venus-Earth-Jupiter: best route 33,654 m/s vs 10,959 m/s reference; see `00_Testing/tests.md`). Allocate the seed budget fairly across first-leg nodes and levels instead of first-come in depth-first order, and revisit the first-leg window; measure with `--coverage-multi`.

## Integration Tests

- [x] Give each integration test an explicit time budget and fail on overrun instead of relying on the ctest default.
- [x] Check result quality, not only constraint validity: best delta-v against a reference, and that several distinct near-optimal routes are returned.
- [x] Add per-test timing output and a recorded baseline so slowdowns are caught.
- [x] Cover the `.itin`/`.itins` file tests, which are not part of the 10 reported tests.
- [x] Add a UI-free check for the porkchop group list (select/deselect all, toggle keeps scroll position).
## KSP Integration

Goal: read the game state from Kerbal Space Program and write planned burns back as maneuver nodes. Nothing is implemented yet; origin/adding-system-config-creator has an earlier "ksp connector" start that has not been reviewed.

- [ ] Use kRPC (a KSP mod that runs a server inside the game) rather than a custom plugin. Clients exist for C (C-nano), C++ and others; the protocol is protobuf over TCP, so KMAT could also speak it directly. Check how well the C-nano client handles streams and a Windows socket transport before choosing.
- [ ] Import bodies from the running game (CelestialBody: gravitational parameter, radius, rotation period, atmosphere depth, orbit, satellites) into a system config, replacing hand-written files and covering modded systems. Celestial_Systems/stock_system.cfg already uses KSP units (units = M_DEG_PA, mean_anomaly_ut0 in radians, ut0 = 0).
- [ ] Read the active vessel's orbit and the game time (Vessel, Orbit, SpaceCenter ut) as the start of an itinerary. KSP time is seconds from the game epoch, KMAT uses Julian dates; reuse the Kerbal date conversion.
- [ ] Write each departure, powered-flyby and deep-space burn as a maneuver node (Control.add_node, Node). Burns must be expressed along the vessel's own orbit at that time, and KSP's node vector order is radial, normal, prograde (KMAT's is different). KSP only shows nodes along its own patched-conic path, so a multi-flyby plan needs its nodes added one at a time as the earlier ones come into range.
- [ ] Moon-assisted arrival has no node equivalent: export the capture burn left after the assist, plus the aiming burn.
- [ ] Optional: Kerbal Alarm Clock alarms for burn times, and the Drawing and UI APIs to show the planned trajectory and a small panel inside the game. Mod integrations (KAC, RemoteTech and others) only work when the player has those mods installed.
- [ ] Fallbacks if kRPC is not wanted: a custom C# KSP plugin that exposes the same data over a local socket or files in GameData, or reading and writing persistent.sfs while the game is closed (vessel orbits and flight-plan maneuvers).
