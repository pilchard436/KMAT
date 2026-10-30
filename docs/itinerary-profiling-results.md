# Itinerary profiling results

This document records how much faster the itinerary search has become, how each
speedup was achieved, and the measurements behind them. It has the optimization
history, the 32-configuration option matrices (baseline, an intermediate point,
and current), and the experiments behind individual changes. The CPU-side
changes of October 2026 and the ideas that were tried and rejected are described
in detail in [`itinerary-cpu-speedup.md`](./itinerary-cpu-speedup.md).

## Summary

On the Earth-to-Saturn profile scenario (366 daily departures), measured from
the first recorded matrix (2026-10-21) to current `main` (2026-10-29):

| Configuration | Baseline | Current | Speedup |
|---|---:|---:|---:|
| Fly-By, all five options on (mask 31) | 502.2 s | 62.6 s | 8.0× |
| Capture, all five options on (mask 31) | 566.9 s | 63.9 s | 8.9× |
| Fly-By, no options (mask 00) | 58.3 s | 7.7 s | 7.6× |
| Fly-By, resonant flybys only (mask 01) | 439.2 s | 49.1 s | 8.9× |
| All 64 configurations, summed | 18,870 s | 2,279 s | 8.3× |

Going back to the legacy Lambert solver, the same base scenario (no options) on
the parent repository's `main` takes 43.6 s of search with no optimization
stage; current `main` does more work (powered-flyby seeds and route
optimization) in 7.7 s, about 5.7× faster end to end (see
[Progress on the base scenario](#progress-on-the-base-scenario)).

The largest steps were the parallel optimizer stages (config 00 −46%), the
Kepler solver with its companion changes (coverage run −57%), and the October 29
hot-spot work (config 00 −57%, resonant config 01 −71%). Single Lambert solves
are about 16× faster since the Izzo solver replaced the legacy one.

## Optimization history

### 2023–2025: original solver and threading

These effects are quoted from commit messages. No shared benchmark existed at
the time, so they cannot be multiplied together or compared with the 2026
measurements below.

| Date | Commit | Optimization | How | Stated effect |
|---|---|---|---|---|
| 2023-08-16 | `03a38ea` | Launch-profile parameter calculation (launch calculator) | Runs in threads | ~4–5× |
| 2023-12-03 | `01d85c9` | Porkchop size reduction | Multi-threaded | ~2× |
| 2023-12-05 | `09d0e71` | Final porkchop generation | Multi-threaded | ~3× in a first test |
| 2023-12-12 | `e20196d` | Orbit propagation | Starts from a better guess instead of the origin point | ~4× overall |
| 2023-12-12 | `c7a7e6b` | Transfer orbit calculation | Larger initial step size | 2–3× |
| 2023-12-14 | `b92aaa1` | Porkchop generation | Thread pool with per-thread reduction instead of many ad hoc threads | 30–50% |
| 2024-01-27 | `c36b2ba` | Double swing-by search (later removed) | dθ instead of dt | 17% |
| 2024-01-28 | `9d85ff3` | Double swing-by search | Algorithm cleanup | Up to 20–30× |
| 2024-01-29 | `b291da7` | Double swing-by search | Gradient descent for the minimum | ~5× |
| 2024-02-18 | `c98f2de` | Lambert's problem | New algorithm | 3× over the original |
| 2024-03-17 | `a31bd1a` | Transfer search | One thread per departure date | Not stated |
| 2025-03-20 to 04-01 | `3b2b19c`, `e7fe159`, `a95ecf2`, `f53b319` | Threading and large porkchops | Cross-platform threads, quicksort fix for large porkchops, thread count cut to 32 for a responsive cancel, sequence calculation forced to one thread after multithreading problems | Not stated (the last one trades speed for correctness) |

orbitlib's Lambert solver was introduced on 2025-08-28 (orbitlib `6fc6898`) and
remained the search's solver until October 2026; the following sections call it
the legacy solver.

### October 2026: measured steps

| Date | Commit | Optimization | How | Measured effect |
|---|---|---|---|---|
| 10-13 to 10-16 | orbitlib `4563fc4`, `b5cf08c`, `b279cbf`; KMAT `72e870a` | Izzo Lambert solver replaces the legacy solver | The Izzo formulation, with a cancellable and a multi-revolution API, becomes the only solver; the legacy solver and its setting are removed | Legacy 0.27 s vs Izzo 0.017 s per 96,000 solves, 15.1–16.9× per solve (three runs, single thread, rebuilt from history on 10-29 at orbitlib `0ead08e`). In that benchmark the legacy solver's largest propagated endpoint miss was 0.24 against 5.6e-6 for Izzo. End-to-end effect not measured |
| 10-16 | `7c7ae27` | Resonant search | Stall bail-out in the flyby iteration, no resonance from the arrival body, fewer roots, coarser sampling | Not benchmarked |
| 10-21 | `4eb887f` | Option matrix recorded | – | Baseline: [matrices below](#matrix-results) |
| 10-25 | `50bc3ee` | Parallel optimizer stages, lock-free cancellation reads | Selected-candidate refinement, route scoring and rejected-route repair run in worker pools; Fly-By and Capture optimization run concurrently; the cancellation counter is read atomically instead of under a mutex | Config 00: 58.3 s → 31.4 s. All options: Fly-By 502.2 s → 373.6 s, Capture 566.9 s → 379.8 s. Per-stage figures in [Experiments](#experiments) |
| 10-25 | `a1f9da6`, `cba6d70`, orbitlib `eea3508` | Kepler solver | Newton iteration on Kepler's equation for elliptic orbits; `osv_from_elements` stays in eccentric anomaly instead of round-tripping through the true anomaly | 5.6× per propagation call (about 2,860 ns → 509 ns); coverage run 88.7 s → 47.7 s → 45.7 s |
| 10-25 | `bc11951`, `55f69fb`, `a8301c1` | Powered-flyby solves and root search | The SOI correction freezes the periapsis choice after iteration 4; the golden-section search reuses one interior point; the root search stops when the step is below 1 s | Coverage run 45.7 s → 37.9 s; solves hitting the 60-iteration limit 4,544 → 84 in one run; about 3% more on Saturn |
| 10-25 | `68af5b6`, `7e3bb25` | Seed budget 512 → 2,048, shared per body | Deliberately adds work for coverage; the faster code pays for it | Jupiter 2.7 s → 8.1 s at the new budget; Saturn at 2,048 takes 14 s against 28.1 s for the original 512-budget search |
| 10-24 / 10-25 | `e54d006`, `d991abc` | Cached per-step Porkchop delta-v values; parallel delta-v preparation when saving | Analyzer and file I/O paths, not the search | Not benchmarked |
| 10-29 | `94a1ecd`, orbitlib `f36116f` | Cheaper propagation, powered flyby, pruning and resonant scan | Cached per-orbit constants and `sincos` in `osv_from_elements`; binary search plus cached elements in `osv_from_ephem`; radius-independent geometry hoisted and a magnitude-only radius search in `calculate_powered_flyby`; delta-v pruning shares ancestor costs and caches flyby burns; the resonant scan propagates once per date, solves both multi-revolution branches together, refines with the Illinois method and splits brackets at the transfer-plane flip | Config 00: 21.0 s → 9.1 s. Config 01: 185 s → 53 s. Saturn resonant mission benchmark search: 86.4 s → 29.9 s. Per-change figures in [`itinerary-cpu-speedup.md`](./itinerary-cpu-speedup.md#changes) |
| 10-29 | `fc6ed4a` | Link-time optimization | `-flto` for Release and RelWithDebInfo; `-O3` and `-march=x86-64-v3` measured and rejected | Config 00: 9.4 s → 7.9 s (−16%). Config 01: 54.8 s → 51.2 s (−6.5%). Results identical |

### Progress on the base scenario

Earth to Saturn, 366 daily departures, no options, Fly-By final. Each row is one
sample from the benchmark named in its last column, so rows are comparable as a
trend but were not all produced by one script.

| State | Base time | Notes | Source |
|---|---:|---|---|
| Parent repository `main` (Zange10/KMAT `09bb2a3`, 2026-04-17): legacy Lambert solver, step-halving propagator, unpowered flybys only | 43.56 s | Search only, no optimization stage; 9,348 itineraries / 18,455 nodes; mean of 43.48, 43.57 and 43.63 s | Same scenario run through the parent's search API, built `-O2` |
| 10-21 baseline (Izzo solver, powered-flyby search and optimizer) | 58.27 s | 38.98 s search + 18.67 s optimization; 9,233 itineraries | Matrix |
| 10-25 after parallel stages and lock-free cancellation | 31.42 s | Same itineraries | Matrix |
| 10-25 after the Kepler solver, solve changes and seed budget 2,048 | 21.0 s | The budget raise adds work that the faster code absorbs | `itinerary-cpu-speedup.md` |
| 10-29 after hot-spot work | 9.1 s | – | `itinerary-cpu-speedup.md` |
| 10-29 current (LTO) | 7.69 s | 9,300 itineraries / 18,404 nodes | Matrix |

Current `main` does more work than the parent run: it adds powered-flyby seeds,
route scoring, repair and refinement, and uses a larger seed budget. The 5.7×
end-to-end gain therefore understates the speedup per unit of work.

## Scenario and method

All matrices use the same UI-free Earth-to-Saturn scenario with Earth, Mars,
Jupiter and Saturn as candidate bodies (Vesta and Ceres excluded). The search
uses 366 daily departures from JD 2461325.50 through 2461690.50, an arrival
cutoff of JD 2466097.50, a maximum duration of 4407 days, and 7282 m/s
departure, satellite and total delta-v limits on the ephemeris system
(`solar_system_ephem.cfg`).

The five option bits are:

| Abbreviation | Option |
|---|---|
| R | Resonant flybys |
| D | Deep-space maneuvers (DSM) |
| M | Moon-aware flybys |
| A | Moon assist on arrival |
| C | Custom starting orbit |

The custom orbit is circular at Earth radius plus atmospheric altitude plus
50 km. The Fly-By matrix uses `TF_FLYBY` and the Capture matrix `TF_CAPTURE`
(arrival capture delta-v counts toward the limits).

Every figure is wall-clock seconds from one fresh process, not a median of
repeated runs, so small differences can be affected by machine load and run
order. All runs used the default 15-worker limit on an AMD Ryzen 7 9800X3D
(8 cores, 16 logical processors; KMAT reserves one) under Windows with GCC
MinGW, search profiling enabled, and passed full-profile validation. Baseline and
Oct 25 builds are `RelWithDebInfo`; the current build adds LTO and uses GCC 16.2.
`Calculation total` covers search, optimization/refinement and filtering;
initialization is 0.000 s throughout.

To reproduce a row, configure with `-DKMAT_ENABLE_SEARCH_PROFILE=ON` and run, from
a directory directly under the repository root (the runner opens
`../Celestial_Systems/solar_system_ephem.cfg`),
`KMATSearchIntegration.exe --full-profile-config <mask>` for Fly-By or
`--full-profile-capture-config <mask>` for Capture, where `<mask>` is 0 to 31.

## Matrix results

Three points in time are shown. *Baseline* is the 2026-10-21 matrix. *Oct 25*
is the 2026-10-25 re-run at the parallelization commit (before the Kepler
solver); every itinerary and node count matched the baseline. *Current* is
2026-10-29 `main`, with search, optimization/refinement and filtering shown. The
speedup is baseline total divided by current total. Itinerary and node counts
are current; they are higher than the baseline's (for example 9,300 / 18,404
against 9,233 / 18,261 for Fly-By mask 00) because the seed budget was raised
from 512 to 2,048 and the search later changed, not because of the speedups.

### Fly-By final

| ID | Enabled | Baseline total | Oct 25 total | Search | Optimization/refinement | Filtering | Current total | Speedup vs baseline | Itineraries / nodes |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 00 | — | 58.27 | 31.42 | 7.28 | 0.37 | 0.03 | 7.69 | 7.6× | 9,300 / 18,404 |
| 01 | R | 439.18 | 362.18 | 48.16 | 0.38 | 0.58 | 49.12 | 8.9× | 9,320 / 18,468 |
| 02 | D | 65.90 | 32.95 | 7.43 | 0.58 | 0.03 | 8.03 | 8.2× | 9,300 / 18,404 |
| 03 | R, D | 447.96 | 364.81 | 48.27 | 0.59 | 0.58 | 49.44 | 9.1× | 9,320 / 18,468 |
| 04 | M | 62.54 | 34.95 | 9.00 | 0.43 | 0.05 | 9.47 | 6.6× | 9,256 / 18,283 |
| 05 | R, M | 427.84 | 362.04 | 49.85 | 0.40 | 0.11 | 50.36 | 8.5× | 9,275 / 18,341 |
| 06 | D, M | 76.11 | 34.87 | 8.98 | 0.68 | 0.05 | 9.71 | 7.8× | 9,256 / 18,283 |
| 07 | R, D, M | 442.57 | 357.02 | 49.85 | 0.67 | 0.11 | 50.63 | 8.7× | 9,275 / 18,341 |
| 08 | A | 130.81 | 46.62 | 16.96 | 0.38 | 0.07 | 17.40 | 7.5× | 9,300 / 18,404 |
| 09 | R, A | 496.23 | 367.24 | 57.25 | 0.37 | 0.62 | 58.24 | 8.5× | 9,320 / 18,468 |
| 10 | D, A | 140.25 | 47.50 | 16.99 | 0.58 | 0.07 | 17.63 | 8.0× | 9,300 / 18,404 |
| 11 | R, D, A | 504.89 | 368.23 | 57.32 | 0.59 | 0.62 | 58.54 | 8.6× | 9,320 / 18,468 |
| 12 | M, A | 138.11 | 49.41 | 18.67 | 0.43 | 0.08 | 19.18 | 7.2× | 9,256 / 18,283 |
| 13 | R, M, A | 502.36 | 370.77 | 59.11 | 0.41 | 0.15 | 59.66 | 8.4× | 9,275 / 18,341 |
| 14 | D, M, A | 152.31 | 50.61 | 18.60 | 0.69 | 0.08 | 19.37 | 7.9× | 9,256 / 18,283 |
| 15 | R, D, M, A | 516.34 | 371.99 | 60.44 | 0.67 | 0.15 | 61.26 | 8.4× | 9,275 / 18,341 |
| 16 | C | 59.41 | 31.96 | 9.69 | 0.63 | 0.04 | 10.36 | 5.7× | 5,898 / 11,851 |
| 17 | R, C | 424.03 | 354.85 | 50.53 | 0.62 | 0.62 | 51.77 | 8.2× | 5,927 / 11,931 |
| 18 | D, C | 70.28 | 32.75 | 9.77 | 0.94 | 0.04 | 10.75 | 6.5× | 5,898 / 11,851 |
| 19 | R, D, C | 434.95 | 353.70 | 50.61 | 0.93 | 0.62 | 52.16 | 8.3× | 5,927 / 11,931 |
| 20 | M, C | 63.55 | 34.19 | 11.39 | 0.65 | 0.04 | 12.09 | 5.3× | 5,890 / 11,820 |
| 21 | R, M, C | 428.02 | 356.03 | 52.32 | 0.65 | 0.12 | 53.08 | 8.1× | 5,918 / 11,896 |
| 22 | D, M, C | 78.82 | 35.49 | 11.33 | 1.02 | 0.04 | 12.40 | 6.4× | 5,890 / 11,820 |
| 23 | R, D, M, C | 444.72 | 357.25 | 52.22 | 1.01 | 0.12 | 53.35 | 8.3× | 5,918 / 11,896 |
| 24 | A, C | 117.83 | 47.56 | 19.37 | 0.64 | 0.07 | 20.07 | 5.9× | 5,898 / 11,851 |
| 25 | R, A, C | 481.91 | 368.26 | 60.33 | 0.63 | 0.65 | 61.62 | 7.8× | 5,927 / 11,931 |
| 26 | D, A, C | 128.79 | 48.54 | 19.40 | 0.94 | 0.07 | 20.41 | 6.3× | 5,898 / 11,851 |
| 27 | R, D, A, C | 494.06 | 369.13 | 59.70 | 0.94 | 0.65 | 61.29 | 8.1× | 5,927 / 11,931 |
| 28 | M, A, C | 122.26 | 49.89 | 21.02 | 0.65 | 0.08 | 21.75 | 5.6× | 5,890 / 11,820 |
| 29 | R, M, A, C | 486.97 | 370.91 | 61.47 | 0.67 | 0.15 | 62.29 | 7.8× | 5,918 / 11,896 |
| 30 | D, M, A, C | 137.75 | 51.21 | 21.00 | 1.03 | 0.08 | 22.10 | 6.2× | 5,890 / 11,820 |
| 31 | R, D, M, A, C | 502.21 | 373.61 | 61.43 | 1.02 | 0.15 | 62.60 | 8.0× | 5,918 / 11,896 |

### Capture final

| ID | Enabled | Baseline total | Oct 25 total | Search | Optimization/refinement | Filtering | Current total | Speedup vs baseline | Itineraries / nodes |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 00 | — | 55.74 | 31.49 | 7.38 | 0.30 | 0.04 | 7.72 | 7.2× | 1,658 / 3,614 |
| 01 | R | 440.66 | 351.33 | 48.22 | 0.30 | 0.13 | 48.65 | 9.1× | 1,662 / 3,624 |
| 02 | D | 65.94 | 31.80 | 7.36 | 0.52 | 0.04 | 7.92 | 8.3× | 1,658 / 3,618 |
| 03 | R, D | 449.44 | 352.39 | 48.20 | 0.51 | 0.14 | 48.85 | 9.2× | 1,662 / 3,627 |
| 04 | M | 60.35 | 33.35 | 8.98 | 0.35 | 0.04 | 9.37 | 6.4× | 1,651 / 3,586 |
| 05 | R, M | 444.79 | 354.54 | 50.16 | 0.34 | 0.12 | 50.62 | 8.8× | 1,655 / 3,596 |
| 06 | D, M | 74.16 | 34.57 | 9.02 | 0.63 | 0.05 | 9.70 | 7.6× | 1,651 / 3,592 |
| 07 | R, D, M | 457.82 | 356.38 | 50.06 | 0.62 | 0.12 | 50.80 | 9.0× | 1,655 / 3,602 |
| 08 | A | 147.00 | 51.47 | 17.04 | 1.21 | 0.08 | 18.33 | 8.0× | 1,692 / 3,686 |
| 09 | R, A | 525.85 | 372.32 | 57.52 | 1.21 | 0.14 | 58.87 | 8.9× | 1,695 / 3,692 |
| 10 | D, A | 171.52 | 54.24 | 17.02 | 1.78 | 0.08 | 18.88 | 9.1× | 1,692 / 3,688 |
| 11 | R, D, A | 558.01 | 375.34 | 57.62 | 1.79 | 0.14 | 59.56 | 9.4× | 1,695 / 3,695 |
| 12 | M, A | 142.96 | 53.81 | 18.67 | 1.24 | 0.08 | 19.99 | 7.2× | 1,685 / 3,658 |
| 13 | R, M, A | 528.64 | 375.37 | 59.36 | 1.23 | 0.15 | 60.74 | 8.7× | 1,688 / 3,664 |
| 14 | D, M, A | 179.03 | 56.97 | 18.65 | 1.88 | 0.08 | 20.61 | 8.7× | 1,685 / 3,661 |
| 15 | R, D, M, A | 565.41 | 378.38 | 59.35 | 1.88 | 0.14 | 61.38 | 9.2× | 1,688 / 3,667 |
| 16 | C | 60.96 | 31.93 | 9.72 | 0.57 | 0.05 | 10.34 | 5.9× | 1,533 / 3,342 |
| 17 | R, C | 447.33 | 352.06 | 50.51 | 0.61 | 0.14 | 51.25 | 8.7× | 1,540 / 3,359 |
| 18 | D, C | 72.05 | 33.01 | 9.70 | 0.89 | 0.05 | 10.64 | 6.8× | 1,533 / 3,349 |
| 19 | R, D, C | 457.40 | 353.70 | 50.44 | 0.93 | 0.13 | 51.50 | 8.9× | 1,540 / 3,365 |
| 20 | M, C | 65.69 | 34.60 | 11.32 | 0.61 | 0.05 | 11.99 | 5.5× | 1,526 / 3,314 |
| 21 | R, M, C | 450.92 | 355.85 | 52.34 | 0.64 | 0.14 | 53.12 | 8.5× | 1,534 / 3,335 |
| 22 | D, M, C | 81.13 | 35.92 | 11.36 | 1.00 | 0.06 | 12.42 | 6.5× | 1,526 / 3,322 |
| 23 | R, D, M, C | 466.96 | 358.05 | 52.26 | 1.03 | 0.14 | 53.43 | 8.7× | 1,534 / 3,344 |
| 24 | A, C | 139.93 | 52.51 | 19.40 | 1.46 | 0.09 | 20.95 | 6.7× | 1,547 / 3,372 |
| 25 | R, A, C | 527.14 | 373.29 | 59.65 | 1.46 | 0.15 | 61.27 | 8.6× | 1,554 / 3,385 |
| 26 | D, A, C | 174.51 | 55.33 | 19.41 | 2.13 | 0.09 | 21.63 | 8.1× | 1,547 / 3,376 |
| 27 | R, D, A, C | 558.91 | 376.17 | 59.72 | 2.16 | 0.16 | 62.03 | 9.0× | 1,554 / 3,390 |
| 28 | M, A, C | 144.45 | 54.96 | 21.03 | 1.48 | 0.09 | 22.60 | 6.4× | 1,540 / 3,344 |
| 29 | R, M, A, C | 529.11 | 377.45 | 61.51 | 1.46 | 0.16 | 63.12 | 8.4× | 1,547 / 3,357 |
| 30 | D, M, A, C | 182.24 | 58.23 | 21.18 | 2.24 | 0.09 | 23.51 | 7.8× | 1,540 / 3,349 |
| 31 | R, D, M, A, C | 566.91 | 379.76 | 61.48 | 2.22 | 0.16 | 63.86 | 8.9× | 1,547 / 3,362 |

### Totals and option averages

| Final transfer | Baseline sum | Oct 25 sum | Current sum | Baseline mean | Current mean | Current mean search / optimization / filtering |
|---|---:|---:|---:|---:|---:|---|
| Fly-By | 9,077.243 s | 6,487.961 s | 1,133.810 s | 283.664 s | 35.432 s | 34.553 / 0.662 / 0.215 s |
| Capture | 9,792.985 s | 6,546.562 s | 1,145.642 s | 306.031 s | 35.801 s | 34.551 / 1.146 / 0.104 s |

Optimization/refinement was 15–118 s per configuration in the baseline and is at
most 2.24 s now; search is about 97% of the current mean calculation time.

Each off/on mean averages 16 configurations, with the other four options
balanced across their on/off states (calculation seconds, off → on):

| Option | Fly-By baseline | Fly-By current | Capture baseline | Capture current |
|---|---:|---:|---:|---:|
| Resonant flybys | 100.19 → 467.14 (+366.3%) | 14.90 → 55.96 (+275.6%) | 113.61 → 498.46 (+338.8%) | 15.41 → 56.19 (+264.6%) |
| DSM | 277.46 → 289.87 (+4.5%) | 35.26 → 35.60 (+1.0%) | 294.47 → 317.59 (+7.9%) | 35.56 → 36.05 (+1.4%) |
| Moon-aware flybys | 280.92 → 286.41 (+2.0%) | 34.66 → 36.21 (+4.5%) | 303.27 → 308.79 (+1.8%) | 34.90 → 36.70 (+5.2%) |
| Moon assist on arrival | 251.51 → 315.82 (+25.6%) | 30.65 → 40.21 (+31.2%) | 259.46 → 352.60 (+35.9%) | 30.52 → 41.08 (+34.6%) |
| Custom starting orbit | 287.61 → 279.72 (−2.7%) | 34.11 → 36.76 (+7.8%) | 304.21 → 307.85 (+1.2%) | 34.50 → 37.10 (+7.6%) |

## Baseline cost breakdown

These are summed worker-seconds per configuration from the baseline matrix, not
wall-clock seconds. They showed that the flyby search was 95% of the recursive
search, which directed the work above.

| Component | Fly-By mean | Capture mean |
|---|---:|---:|
| Initial grid | 2.432 | 2.472 |
| Recursive search | 3,335.049 | 3,550.239 |
| End-node collection | 0.044 | 0.053 |
| Delta-v pruning | 158.895 | 175.147 |
| Flyby search | 3,173.395 | 3,371.623 |
| Route expansion/other | 2.715 | 3.417 |

| Flyby body | Fly-By worker s/run | Capture worker s/run |
|---|---:|---:|
| Earth | 1,058.609 | 1,126.649 |
| Mars | 1,842.252 | 1,953.951 |
| Jupiter | 171.966 | 183.586 |
| Saturn | 100.568 | 107.438 |

## Experiments

Each experiment compares two builds on Fly-By profile configuration 00 (no
options) at the time, two fresh-process runs per variant unless noted. The
itinerary and node counts matched in every pair. Parallel repair uses a per-slot
random stream, which changed the random sequence and moved the Capture
improved-route count from 527 to 526.

| Change | Measure | Before | After | Effect |
|---|---|---:|---:|---|
| Selected-candidate refinement in a worker pool | Refinement / total | 10.775 s / 55.785 s | 0.985 s / 45.390 s | 10.9× on refinement, −18.6% total |
| Route scoring in a worker pool | Scoring + merge + repair / total | 6.96 s / 46.349 s | 2.77 s / 42.761 s | 2.5× on the stage, −7.7% total |
| Rejected-route repair in a worker pool | Repair Fly-By, Capture / total | 2.82 s, 2.17 s / 44.835 s | 0.48 s, 0.43 s / 42.510 s | 5.9× and 5.1× on repair, −5.2% total |
| Fly-By and Capture optimization run concurrently (both-modes config 0) | Total | 45.643 s | 45.157 s | −1.1%; Capture is a small share and the worker budget is split |
| Cancellation counter read: mutex → atomic (before: one sample, after: mean of 3) | Search / total | 38.633 s / 40.560 s | 30.168 s / 32.108 s | −21.9% search, −20.8% total; the shared lock was a significant source of contention |

The scoring figure includes candidate merging and repair because the timer
covered all three at that point; the current build reports them separately. A
separate two-run measurement put global ranking/merge at about 0.9 ms (Fly-By) and
0.2 ms (Capture) and challenge selection at about 3.2 ms and 0.6 ms, against
route scoring of 0.84 s and 0.97 s, so parallelizing ranking or challenge
selection is not worthwhile.

### Worker scaling

The same configuration was run once at each worker limit with
`--full-profile-worker-scale <workers> 0` (9,233 itineraries and 18,261 nodes in
every run). The 15-worker row is the mutex-read baseline of the table above.

| Worker limit | Search (s) | Optimization/refinement (s) | Filtering (s) | Calculation total (s) |
|---:|---:|---:|---:|---:|
| 1 | 369.647 | 18.062 | 0.383 | 388.094 |
| 2 | 186.514 | 8.912 | 0.204 | 195.631 |
| 4 | 95.851 | 4.696 | 0.204 | 100.752 |
| 8 | 54.807 | 2.884 | 0.226 | 57.918 |
| 12 | 39.248 | 1.987 | 0.224 | 41.459 |
| 15 | 38.633 | 1.702 | 0.224 | 40.560 |

Search was 9.6× faster at 15 workers than at 1, but 15 workers cut search time by
only 1.6% against 12, so the 15-worker cap is already near the useful limit and
raising it above the logical-processor budget is not supported by these results.

## Interpretation notes

- Transfer type changes feasibility and optimization work, so Capture is not
  uniformly slower: it averaged 7.9% slower than Fly-By across the baseline
  matrix and averages 1.0% slower now.
- Resonant flybys dominate the cost (about 3.8× in the current matrix),
  followed by moon assist on arrival (about 1.3×). DSM adds 1–1.4%.
- The custom starting orbit changes route feasibility and counts (Fly-By mask
  31 finds 5,918 itineraries, mask 15 without the orbit 9,275), so its time
  difference is not overhead alone.
- Baseline moon-assist Capture runs spent about 13–15 s outside the calculation
  timer in post-search validation; in the current runs the outer-process time is
  within 0.01 s of the calculation total.
- Whole-matrix comparisons mix several changes and should not be attributed to
  one of them; the experiments above and
  [`itinerary-cpu-speedup.md`](./itinerary-cpu-speedup.md) isolate individual
  changes.
