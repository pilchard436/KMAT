# Itinerary search CPU speedup

This guide records the CPU-side changes that made the itinerary flyby search
faster without changing which routes it finds, how each was measured, and which
ideas were tried and rejected. For the option-matrix timings that motivated the
work, see [`itinerary-profiling-results.md`](./itinerary-profiling-results.md).

## Why

Enabling more search options exhausts the per-departure powered-seed budget
before the search reaches some route families. A larger budget fixes that but
made the search too slow, so the goal was to make each search step cheaper.
Profiling showed the flyby search takes 83-97% of recursive-search time, and
seed sampling is only 1-2% of the work.

## Changes

| # | Change | Where | Effect |
|---|---|---|---|
| 1 | Newton solve of Kepler's equation for elliptic orbits in `propagate_orbit_time`; the old step-halving search remains as a fallback and for `e >= 1` | orbitlib `src/orbit.c` | 5.6x faster per call (about 2,860 ns to 509 ns); coverage run 88.7 s to 47.7 s |
| 2 | `osv_from_elements` builds position and velocity directly from the eccentric anomaly, with the old path as fallback | orbitlib `src/orbit.c` | About 4% (47.7 s to 45.7 s) |
| 3 | SOI-correction freeze: after iteration 4 the flyby periapsis radius and side are frozen (`POWERED_FREEZE_ITERATION`), falling back to the free optimum if the frozen choice becomes infeasible | `orbit_calculator/powered_route_objective.c` | Solves that hit the 60-iteration limit fell from 4,544 to 84 in one integration run |
| 4 | Golden-section search in `calculate_powered_flyby` reuses one interior point per step | orbitlib `src/transfer.c` | One radius evaluation per step instead of two; with #3, 45.7 s to 37.9 s |
| 5 | Root search in `find_viable_flybys` stops when the step falls below 1 s, instead of only at exactly 0 | `orbit_calculator/flyby_search.c` | About 3% on Saturn |
| 6 | `osv_from_elements` keeps the perifocal rotation and the mean anomaly at epoch of recently used orbits in a per-thread cache, starts Newton from one step past M, uses `sincos`, and stops once the step is below 1e-8 | orbitlib `src/orbit.c` | 1,900 to 740 cycles per call; agrees with the old path to 1e-15 |
| 7 | `osv_from_ephem` finds the nearest entry by binary search (the tables hold 1,800 monthly entries) and propagates the cached elements of that entry through the same fast path instead of the state, element, true-anomaly, state round trip | orbitlib `src/ephemeris.c` | About 6,400 to 1,300 cycles per call; propagation fell from 51% to 24% of the flyby search in the ephemeris scenario |
| 8 | `calculate_powered_flyby` hoists the radius-independent geometry out of the per-radius evaluation, caches the 64-point log grid per body, and searches the radius with a magnitude-only evaluator (the two mirror sides have the same burn), building the full result once at the chosen radius | orbitlib `src/transfer.c` | 45,800 to 26,200 cycles per full call; per-case results identical |
| 9 | Delta-v pruning accumulates the shared ancestor costs once per parent and caches each node's plain flyby burn (keyed on the velocities it was computed from) for deeper pruning passes; only the search worker that owns the tree uses the cache | `orbit_calculator/itin_tool.c` | Pruning 21.2 to 7.5 worker-seconds (81% of its burns were repeats) |
| 10 | The resonant scan propagates the target once per sample date and solves all leg variants from it, with both branches of each revolution count sharing geometry and the minimum-time search (`calc_lambert3_revs_pair_cancellable`); multi-revolution solves reject times below N*pi before iterating | `orbit_calculator/resonant_flybys.c`, `leg_variants.c`, orbitlib `src/transfer.c` | Scan propagation 1,400 to 68 Gcyc, scan Lambert 1,266 to about 650 Gcyc on Saturn |
| 11 | Resonant roots are refined with the Illinois method, and brackets that straddle the transfer-plane flip (transfer angle through 0 or 180 degrees, which every resonant window contains) are split there instead of being bisected onto the discontinuity | `orbit_calculator/resonant_flybys.c` | 62% of brackets were not roots and ran 40 Lambert solves each; refinement fell from 1,852 to about 250 Gcyc on Saturn |

Why the freeze works: the damped SOI fixed point converges geometrically, then
jumps when the discrete periapsis choice flips between near-equal-cost optima.
Freezing after iteration 2 locked in bad radii (differences up to 1,197 m/s);
8 and 12 still failed the zero-DSM consistency check on one date pair; 4 passed
every check.

Where the time went before changes 6 to 11 (cycle counters inside
`find_viable_flybys`, Earth to Saturn, 366 departures): propagation 51%, seed
selection (`calculate_powered_flyby`) 23%, Lambert 12%, root flyby checks 6%,
root-finder bookkeeping 2%. Each propagation cost about four Lambert solves.
In the resonant Saturn run the resonant search was 78% of all flyby-search
time, split into scan propagation 30%, scan Lambert 27% and refinement 40%.

The resonant return is intrinsically near-degenerate: as the body comes back
through a transfer angle of 0 or 180 degrees, `cross(r0, r1).z` changes sign,
Lambert's lambda flips sign, and the speed difference jumps. A sign change
across that jump is not a root, but bisection spent its full 40 iterations
converging onto it. The scan now records the plane sign per sample, locates
the flip by bisection on the propagated position (no Lambert), refines the
smooth sub-intervals on either side, and, when neither holds a root, still
offers the better side next to the flip to the same `|dv_inf| < 1 m/s` check
so near-exact-period returns are kept.

The burn-versus-periapsis curve of a powered flyby has a V-shaped minimum
where the burn nears zero, so the golden-section bracket must shrink to
roundoff: 20 instead of 32 steps changed sub-metre burns by several m/s and
was reverted.

The old propagator was only accurate to about 1 s of orbital time, so some
integration checks calibrated to that noise were adjusted: the DSM total
comparison, the resonant re-solve cost for maneuver-free routes, and one DSM
model date pair. The integration search test no longer requires rejected-route
repair counters, because its fixture now produces no rejections.

## Results

Coverage benchmark (`KMATSearchIntegration --coverage`, from `build`), one run
per row, 15 workers:

| Case | Original | Now |
|---|---:|---:|
| Coverage run, total wall time | 88.7 s | about 38 s |
| Jupiter, budget 512 | 5.6 s | 2.7 s |
| Jupiter, budget 2,048 | 14.8 s | 8.2 s |
| Saturn, budget 512 | 28.1 s | 9.8 s |

With `--resonant`, original `main` against the current branch:

| Case | Original | Now |
|---|---:|---:|
| Jupiter scale 1.0 | 33 s | 10 s |
| Saturn scale 1.0 | 249 s | 71 s |
| Jupiter scale 1.5 | 24 s | 7 s |
| Jupiter scale 2.0 | 19 s | 7 s |
| Saturn scale 2.0 | 143 s | 41 s |
| Full run (wall time) | 606 s | about 136 s (sum of the rows above) |

The resonant search is still the most expensive option: its 6-day scan costs
about 700 Lambert solves per search for Earth, and Saturn with resonant remains
the slowest case.

### After changes 6 to 11

Same machine (Ryzen 7 9800X3D, 15 workers), one run per row. The profile
configurations are the 366-departure Earth to Saturn scenario of
[`itinerary-profiling-results.md`](./itinerary-profiling-results.md) on the
ephemeris system; the mission benchmarks use orbital elements.

| Case | Before | After |
|---|---:|---:|
| Profile config 0, search wall time | 20.1 s | 9.3 s |
| Profile config 0, calculation total | 21.0 s | 9.1 s |
| Profile config 1 (resonant), calculation total | 185 s | 53 s |
| Jupiter mission benchmark with `--resonant`, search | 16.6 s | 7.3 s |
| Saturn mission benchmark with `--resonant`, search | 86.4 s | 29.9 s |

Config 0 found the same 9,300 itineraries (18,404 nodes instead of 18,401);
best full delta-v 4,676.9 instead of 4,676.2 m/s. Config 1 went from 9,321 to
9,320 itineraries (18,470 to 18,468 nodes). The Saturn resonant benchmark
kept 9,002 itineraries and 37 resonant routes (18,498 nodes instead of
18,503); Jupiter went from 4,844 to 4,823 itineraries with the same 121
resonant routes and best delta-v. Propagation differences are at the 1e-12
level, the powered-flyby changes are per-case identical, and the resonant root
dates move by under a second within the existing 1e-3 m/s tolerance; the
count shifts come from the search amplifying those.

### Effect on results

Itinerary counts and best delta-v shift by at most about 0.3%; the seeds added
are identical and skipped counts are within about 0.01%. Examples: Jupiter
4,526 to 4,523 itineraries and Saturn 8,894 to 8,904 with resonant; Saturn
best delta-v 4711.337 to 4698.910 m/s without resonant (never worse), though
Saturn with resonant moved from 4711.337 to 4711.691 m/s. Full itinerary lists
were not compared route by route. Integration tests are mildly nondeterministic
across runs because of thread timing.

## Raising the seed budget

With the faster search, `POWERED_SEEDS_PER_DEPARTURE` is raised from 512 to
2,048. Jupiter benchmark (Earth, Mars, Jupiter) by budget:

| Budget | Time | Itineraries |
|---:|---:|---:|
| 64 | 0.9 s | 4,402 |
| 128 | 1.1 s | 4,429 |
| 512 | 2.7 s | 4,598 |
| 768 | 3.8 s | 4,715 |
| 1,024 | 4.7 s | 4,890 |
| 1,536 | 6.7 s | 5,056 |
| 2,048 | 8.1 s | 5,056 |
| 4,096 to 16,384 | 11.7 to 16.9 s | 5,056 |

Coverage saturates at about 1,536 for this scenario, so 2,048 adds margin
without wasted work beyond that. At 2,048: Jupiter scale 1.5 finds 3,213
itineraries (2,996 at 512), Saturn scale 1.0 finds 8,925 (8,908) in 14 s
(9.8 s at 512), and Saturn scale 2.0 finds 4,612 (4,592) in 12 s. The
resonant Saturn run is unchanged at about 76 s. Total cost is still below the
original 512-budget search (Saturn 28.1 s). Other scenarios may need more.

The itinerary-operations integration check skips the re-solve consistency
comparison for routes above 20 km/s, because the larger budget stores
extreme-burn powered-seed routes (hundreds of km/s) with no practical value,
where solver noise is unbounded. The powered-search budget check now sets a
small budget explicitly so it still exercises budget exhaustion.

## Tried and rejected

- **OpenCL Lambert batching** was correct but no faster, because seed sampling
  is a small share of the work. It stays on `feature/opencl-itinerary-search`.
  Day-wise GPU parallelism was also ruled out: a departure's tree walk is
  serial, divergent and allocation-heavy, and 400 lanes at GPU scalar FP64
  rates are fewer core-equivalents than the 15 CPU workers.
- **Fewer golden-section steps in `calculate_powered_flyby`** (20 instead of
  32) changed sub-metre burns by several m/s because the minimum is V-shaped.
- **Flyby burn cache in delta-v pruning** gave no measurable gain when first
  tried on the end nodes; measuring showed 81% of the burns were ancestor
  repeats across depths, which the per-node cache of change 9 removes.
- **Illinois false position for the resonant root refinement** alone matched
  bisection within noise; the cost was the spurious brackets at the
  transfer-plane flip, which change 11 handles.
- **Seed selection** is bounded at 4 buckets x 3 evaluations per search (about
  7% typical), and trimming it would change which seeds are chosen.
- **Worker load balancing**: CPU utilization was 84% over a full run, so at
  most about 16% could be recovered.
- **Burn-ranked seed spending**: once under a quarter of a departure's budget
  was left, skipping seeds whose burn exceeded 3x the cheapest seen plus
  200 m/s raised Saturn coverage from 8,982 to 10,903 itineraries, but the
  full Earth to Saturn run took 47 s instead of 21 s, best delta-v got worse
  in one scaled case, and the integration tests lost most of their distinct
  body sequences. Reverted.
- **Equal split of the budget over every child node** (instead of over
  bodies) starved deep routes: distinct body sequences fell from 300 to 33 in
  one integration test. The body-level reserve is used instead.

### Compiler flags

Measured on the Earth to Saturn profile scenario (config 0, three runs each, GCC
16.2 MinGW, `-O2` baseline 9.38 s):

| Flags | Config 0 | Config 1 (resonant) | Nodes (config 0) |
|---|---:|---:|---:|
| `-O2` | 9.38 s | 54.8 s | 18,404 |
| `-O3` | 9.54 s | | 18,404 |
| `-O3 -march=x86-64-v3` | 9.26 s | | 18,405 |
| `-O2 -flto` | 7.89 s | 51.2 s | 18,404 |
| `-O3 -march=x86-64-v3 -flto` | 7.44 s | 47.5 s | 18,402 |

LTO is enabled for Release and RelWithDebInfo (`KMAT_ENABLE_LTO`, on by
default): 16% on the non-resonant search and 6.5% on the resonant one, with
identical results. `-O3` does not help, and `-march=x86-64-v3` only adds a few
percent with FMA-induced differences in the search and a CPU requirement
(AVX2), so neither is used.

## Remaining options

- A coarser resonant scan step or fewer leg variants would be faster but can
  skip narrow sign changes or whole route families.
- Changing the root-finder sampling in `calculus.c` would cut the roughly 59
  evaluations per Earth search, but changes which dates are found; compare
  counts and best delta-v after any tweak.

## Reproducing

Build directly under the repository root (the harness resolves
`../Celestial_Systems`) and run from inside the build folder:

```text
cmake --preset default
cmake --build --preset default
cd build
.\KMATSearchIntegration.exe --coverage            # add --resonant for resonant
```

Per-body flyby statistics need a profile build with
`-DKMAT_ENABLE_SEARCH_PROFILE=ON`. The orbitlib changes are submodule commits
that must be committed there and the pointer updated in this repository.
