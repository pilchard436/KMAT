# Itinerary Calculator:
## Passes if:
- dv-requirements are met with each itinerary
- date requirements are met with each itinerary
- fly-by body_names requirements are met
- has only valid itineraries (Periapsides etc.)
## Tests:
### Stock:
- 1: Kerbin -> Eeloo; 1-001 | 2-001 | 3000; dv_dep = 1500 | Bodies: all 
- 2: Kerbin -> Eeloo; 1-001 | 2-001 | 3000; dv_dep = 1800, dv_arr = 1800 (circ) | Bodies: Eve, Kerbin, Duna, Eeloo
- 3: Kerbin -> Kerbin; 1-001 | 2-001 | 1000; dv_dep = 1800 | Bodies: Eve, Kerbin, Duna, Jool
### RSS (Ephem)
- 4: Earth -> Earth; 1959-01-01 | 1960-01-01 | 1000; dv_dep = 5000 | Bodies: all
- 5: Earth -> Earth; 1959-01-01 | 1960-01-01 | 1000; dv_dep = 5000 | Bodies: Mercury, Venus, Earth
- 6: Earth -> Jupiter; 1967-01-01 | 1968-01-01 | 3000; dv_dep = 4500 | Bodies: Venus, Earth, Mars, Jupiter

# Sequence Calculator:
## Passes if:
- dv-requirements are met with each itinerary
- date requiremetns are met with each itinerary
- each itinerary is in defined sequence
- has only valid itineraries (Periapsides etc.)
### Stock:
- 1: Kerbin -> Eve; 2-001 | 3-001 | 300; dv_dep = 1200; dv_arr = 1500 (circ)
- 2: Kerbin -> Eve -> Duna -> Kerbin; 2-001 | 3-001 | 1000; dv_dep = 1500
### RSS (Ephem)
- 3: Earth -> Mars; 1950-01-01 | 1950-06-01 | 1950-11-01 | 600; dv_dep = 5000; dv_arr = 4000 (circ); dv_tot = 8200
- 4: Earth -> Jupiter -> Saturn -> Uranus -> Neptune; 1977-01-01 | 1978-01-01 | 8000; dv_dep = 7200

# Porkchop Analyzer:
## Passes if:
- has only valid itineraries (Periapsides etc.)
## Tests:
- Load number of different .itins files

# Transfer Planner:
## Passes if:
- itinerary is valid (Periapsides etc.)
## Tests:
- Load number of different .itin files

# Budgeted powered-flyby search (2026-10-15)

Build and run the focused checks from the repository root:

```powershell
cmake --build build --target KMAT KMATSearchIntegration KMATUnitTests -j 6
ctest --test-dir build --output-on-failure -R '^(KMATUnitTests|KMATSearchIntegration)$' --timeout 60
```

The unit fixture checks that an infeasible cheapest estimate falls back to a later
date, duplicates do not consume shortlist capacity, and a wholly infeasible bucket
stops within three evaluations. The search integration checks distinct
speed-changing dates before optimization, both Lambert solvers, and full mission
objective quality against a coarse grid and an unpowered baseline.

The full-year profile also requires that no itinerary is pruned by the final leaf
filter after optimization (`powered_itineraries_pruned == 0`), that repair stays
within its 64-route cap, and that the cutoff challenge refines between 1 and 32
routes. Production runs print a `Rejected-route repair:` line with initial
rejections, sampled/recovered/competing repairs, and cutoff-challenge counts.

The quick Earth-Venus-Jupiter run recomputes each returned route's objective from
its encounter dates and compares it with the objective implied by the stored node
velocities; the worst mismatch must stay within 0.1 m/s. This catches stale
velocities from route write-back: reintroducing the old suffix-only write-back
raises the mismatch from 0.000 to about 849,000 m/s. The same run requires that
rejected routes are repaired, recovered routes reach refinement, and counts stay
consistent (recovered at most attempted, attempted at most rejected). The unit
suite also checks that a 0.5 m/s excess-speed change keeps a powered burn.

Deep-space maneuvers (`allow_dsm`, off by default) are tested in the same run. Each
leg may have one maneuver: the departure velocity changes by a delta-v, the route
coasts for a fraction eta of the leg, and a second Lambert arc reaches the next
encounter. A zero delta-v reproduces the plain route within the evaluator's
convergence noise (checked to 10 m/s), and a maneuver counts toward the satellite
delta-v limit. The checks are:

- fixed-date routes: the optimizer never returns more than the plain cost, its
  returned maneuvers re-evaluate to the same cost, a known route saves more than 100
  m/s, and a tight satellite limit rejects the maneuver;
- full search with and without DSMs on Earth-Venus-Jupiter: the best flyby route must
  not get worse and must improve by at least 50 m/s (measured 26,570.9 to 26,408.5
  m/s), and every route carrying a maneuver must cost at least 1 m/s less than the
  same route recomputed from its dates without it;
- a `.itins` save and reload keeps every maneuver node and the best total delta-v
  (without the loader fix for body id -1 this crashes).

Local Windows RelWithDebInfo measurements, profiling enabled:

| Metric | Branch HEAD before edits | Budgeted implementation |
| --- | ---: | ---: |
| Bounded integration wall time | 12.12 s | 6.68-8.03 s |
| Completed candidate routes | 824 | 646 |
| Valid SOI-scored routes | 539 | 428 |
| Best full objective | 26570.869 m/s | 26570.869 m/s |
| Initial Lambert calls | 712 | 712 |
| Recursive Lambert calls | 42296 | 42169 |

The updated run used 1749 seed feasibility evaluations, 512 powered seeds including
eight extra branches, 15332 full route objective evaluations, and 33 routes with
multiple timing starts. It reported 1237 feasible bucket alternatives omitted by
branching budgets, 396 encounters that skipped powered sampling after exhausting
the departure budget, and zero exhausted per-route refinement budgets. The smaller
candidate set is an explicit runtime/coverage tradeoff; the best objective is
preserved on this fixture. Both focused CTest targets passed;
the unit executable reports 93 checks. These are individual local timings, not a
statistical performance guarantee.

The full-year ephemeris profile (`KMATSearchIntegration --full-profile`) completed
in 61.65 seconds after adding the total powered-seed budget. It passed constraints
for every returned route, with 6763 nodes, 3346 itineraries, 500 corrected routes,
and best corrected objective 4712.109 m/s. It used 847510 initial and 129132019
recursive Lambert solves. Scoring took 1.53 seconds and timing refinement 4.87
seconds. It added 187392 powered seeds across 366 departure dates, skipped powered
sampling at 1764811 encounters after budget exhaustion, and reported 423371
feasible bucket alternatives omitted by branching budgets.

Baseline HEAD and the intermediate version with only an extra-branch budget were
both stopped at approximately 166 seconds without completing a departure date.
The full-profile harness checks returned route constraints and a finite,
within-budget corrected objective instead of requiring a fixed route count.
The new budgets do not bound the existing near-unpowered recursive search, and
these results do not prove exhaustive coverage or preserve every baseline route.
The broad `KMATIntegrationTests` application suite was stopped after exceeding a
minute on the intermediate version and is not claimed as passing.

The final saved mission benchmarks (`KMATSearchIntegration --mission-benchmarks`)
both passed in 46.53 seconds combined: Jupiter returned 4191 itineraries in 13.19
seconds, with best corrected objective 4623.567 m/s; Saturn returned 3090
itineraries in 33.29 seconds, with best corrected objective 4718.541 m/s.

# Parking orbit

The departure ?v of a search can use a custom parking orbit (six classical elements in the system's inertial frame; the departure burn is minimized over the orbit). It is a calculation setting and is not stored in .itins files. Unit tests check circular and polar limits, asymptote recomputation, validity and the active-orbit scope; the search integration checks that every route's stored departure ?v equals the independent burn for the orbit, never falls below the free-plane burn, and is penalized on routes off the orbit's line of nodes. Manual check: Itinerary Calculator > Parking Orbit..., edit the elements and confirm the figure and status line update.
