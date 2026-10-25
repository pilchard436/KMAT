# Itinerary profiling results

## Scenario and method

Both matrices use the same UI-free Earth-to-Saturn scenario with Earth, Mars,
Jupiter, and Saturn as candidate bodies (Vesta and Ceres excluded). The search
uses 366 daily departures from JD 2461325.50 through 2461690.50, an arrival
cutoff of JD 2466097.50, a maximum duration of 4407 days, and 7282 m/s
departure, satellite, and total delta-v limits.

The five option bits are:

| Abbreviation | Option |
|---|---|
| R | Resonant flybys |
| D | Deep-space maneuvers (DSM) |
| M | Moon-aware flybys |
| A | Moon assist on arrival |
| C | Custom starting orbit |

The custom orbit is circular at Earth radius plus atmospheric altitude plus
50 km. Each mask is run in a fresh process. Each reported configuration has one
accepted run; the timing measurements are not repeated medians.

`Search`, `Optimization/refinement`, `Filtering`, and `Calculation total` are
wall-clock seconds. Initialization was 0.000 s in all runs. `Outer process`
includes work outside the calculation timer, including validation. It should
not be treated as the calculation wall time.

## Flyby-final matrix

All 32 configurations passed validation.

| ID | Enabled | Search (s) | Optimization/refinement (s) | Filtering (s) | Calculation total (s) | Outer process (s) | Itineraries / nodes |
|---:|---|---:|---:|---:|---:|---:|---:|
| 00 | — | 38.975 | 18.671 | 0.620 | 58.267 | 58.796 | 9,233 / 18,261 |
| 01 | R | 411.932 | 18.798 | 8.452 | 439.183 | 439.740 | 9,237 / 18,286 |
| 02 | D | 36.790 | 28.458 | 0.649 | 65.897 | 66.451 | 9,233 / 18,261 |
| 03 | R, D | 410.981 | 28.546 | 8.435 | 447.962 | 448.527 | 9,237 / 18,286 |
| 04 | M | 38.168 | 22.676 | 1.696 | 62.541 | 63.083 | 9,190 / 18,143 |
| 05 | R, M | 396.269 | 22.155 | 9.417 | 427.842 | 428.383 | 9,194 / 18,170 |
| 06 | D, M | 37.635 | 36.792 | 1.689 | 76.115 | 76.708 | 9,190 / 18,143 |
| 07 | R, D, M | 397.025 | 36.149 | 9.395 | 442.569 | 443.111 | 9,194 / 18,170 |
| 08 | A | 49.546 | 47.608 | 33.660 | 130.814 | 131.362 | 9,233 / 18,261 |
| 09 | R, A | 407.152 | 47.628 | 41.445 | 496.225 | 496.774 | 9,237 / 18,286 |
| 10 | D, A | 49.576 | 57.023 | 33.654 | 140.254 | 140.800 | 9,233 / 18,261 |
| 11 | R, D, A | 406.251 | 57.250 | 41.389 | 504.891 | 505.441 | 9,237 / 18,286 |
| 12 | M, A | 51.776 | 51.710 | 34.628 | 138.114 | 138.657 | 9,190 / 18,143 |
| 13 | R, M, A | 408.921 | 51.039 | 42.402 | 502.362 | 502.902 | 9,194 / 18,170 |
| 14 | D, M, A | 51.654 | 65.860 | 34.801 | 152.315 | 152.858 | 9,190 / 18,143 |
| 15 | R, D, M, A | 408.867 | 65.092 | 42.382 | 516.341 | 516.885 | 9,194 / 18,170 |
| 16 | C | 36.717 | 21.353 | 1.345 | 59.415 | 61.338 | 5,870 / 11,775 |
| 17 | R, C | 393.873 | 21.095 | 9.058 | 424.026 | 425.953 | 5,878 / 11,810 |
| 18 | D, C | 36.588 | 32.369 | 1.319 | 70.276 | 72.196 | 5,870 / 11,776 |
| 19 | R, D, C | 393.865 | 32.050 | 9.031 | 434.947 | 436.875 | 5,878 / 11,811 |
| 20 | M, C | 38.429 | 23.498 | 1.620 | 63.548 | 65.468 | 5,863 / 11,749 |
| 21 | R, M, C | 395.497 | 23.175 | 9.346 | 428.018 | 429.949 | 5,871 / 11,784 |
| 22 | D, M, C | 38.162 | 39.043 | 1.617 | 78.822 | 80.738 | 5,863 / 11,750 |
| 23 | R, D, M, C | 396.601 | 38.769 | 9.346 | 444.717 | 446.638 | 5,871 / 11,785 |
| 24 | A, C | 50.302 | 42.420 | 25.110 | 117.832 | 119.745 | 5,870 / 11,775 |
| 25 | R, A, C | 406.832 | 42.168 | 32.909 | 481.909 | 483.846 | 5,878 / 11,810 |
| 26 | D, A, C | 50.372 | 53.311 | 25.103 | 128.787 | 130.714 | 5,870 / 11,776 |
| 27 | R, D, A, C | 408.131 | 53.053 | 32.873 | 494.058 | 495.988 | 5,878 / 11,811 |
| 28 | M, A, C | 52.453 | 44.416 | 25.394 | 122.264 | 124.173 | 5,863 / 11,749 |
| 29 | R, M, A, C | 409.640 | 44.243 | 33.092 | 486.974 | 488.888 | 5,871 / 11,784 |
| 30 | D, M, A, C | 52.341 | 60.036 | 25.374 | 137.751 | 139.666 | 5,863 / 11,750 |
| 31 | R, D, M, A, C | 409.432 | 59.704 | 33.070 | 502.207 | 504.125 | 5,871 / 11,785 |

## Capture-final matrix

All 32 final configurations passed validation. The moon-assist-enabled masks
were rerun after correcting the post-search validator to keep arrival moon
assist enabled while checking capture delta-v. The table reports the passing
measurements.

| ID | Enabled | Search (s) | Optimization/refinement (s) | Filtering (s) | Calculation total (s) | Outer process (s) | Itineraries / nodes |
|---:|---|---:|---:|---:|---:|---:|---:|
| 00 | — | 39.604 | 14.849 | 1.289 | 55.743 | 55.842 | 1,627 / 3,519 |
| 01 | R | 418.522 | 16.300 | 5.838 | 440.662 | 440.774 | 1,629 / 3,525 |
| 02 | D | 40.171 | 24.465 | 1.299 | 65.936 | 66.035 | 1,627 / 3,523 |
| 03 | R, D | 419.822 | 24.215 | 5.404 | 449.442 | 449.541 | 1,629 / 3,529 |
| 04 | M | 41.341 | 17.595 | 1.418 | 60.355 | 60.454 | 1,621 / 3,495 |
| 05 | R, M | 422.008 | 17.258 | 5.526 | 444.793 | 444.891 | 1,623 / 3,501 |
| 06 | D, M | 41.048 | 31.697 | 1.418 | 74.164 | 74.262 | 1,621 / 3,497 |
| 07 | R, D, M | 420.974 | 31.340 | 5.502 | 457.817 | 457.915 | 1,623 / 3,503 |
| 08 | A | 53.236 | 85.008 | 8.753 | 146.997 | 162.305 | 1,658 / 3,583 |
| 09 | R, A | 436.376 | 77.334 | 12.142 | 525.853 | 539.856 | 1,661 / 3,594 |
| 10 | D, A | 53.279 | 110.256 | 7.987 | 171.523 | 185.507 | 1,658 / 3,584 |
| 11 | R, D, A | 435.802 | 110.103 | 12.106 | 558.011 | 572.009 | 1,661 / 3,595 |
| 12 | M, A | 55.583 | 79.291 | 8.080 | 142.956 | 156.877 | 1,652 / 3,559 |
| 13 | R, M, A | 437.546 | 78.884 | 12.214 | 528.645 | 542.557 | 1,655 / 3,570 |
| 14 | D, M, A | 55.176 | 115.779 | 8.074 | 179.030 | 192.986 | 1,652 / 3,560 |
| 15 | R, D, M, A | 437.705 | 115.520 | 12.186 | 565.413 | 579.345 | 1,655 / 3,571 |
| 16 | C | 40.313 | 18.368 | 2.276 | 60.958 | 61.452 | 1,508 / 3,261 |
| 17 | R, C | 422.820 | 18.348 | 6.156 | 447.326 | 447.819 | 1,511 / 3,271 |
| 18 | D, C | 40.344 | 29.434 | 2.268 | 72.047 | 72.537 | 1,508 / 3,268 |
| 19 | R, D, C | 421.936 | 29.349 | 6.119 | 457.405 | 457.894 | 1,511 / 3,278 |
| 20 | M, C | 42.256 | 21.048 | 2.389 | 65.695 | 66.182 | 1,502 / 3,236 |
| 21 | R, M, C | 423.741 | 20.912 | 6.265 | 450.919 | 451.412 | 1,505 / 3,246 |
| 22 | D, M, C | 42.007 | 36.745 | 2.380 | 81.133 | 81.620 | 1,502 / 3,241 |
| 23 | R, D, M, C | 424.352 | 36.702 | 5.906 | 466.961 | 467.450 | 1,505 / 3,252 |
| 24 | A, C | 53.454 | 77.860 | 8.613 | 139.928 | 153.158 | 1,525 / 3,295 |
| 25 | R, A, C | 436.157 | 78.488 | 12.492 | 527.138 | 540.386 | 1,528 / 3,305 |
| 26 | D, A, C | 53.721 | 112.154 | 8.638 | 174.515 | 187.731 | 1,525 / 3,297 |
| 27 | R, D, A, C | 433.809 | 112.609 | 12.488 | 558.907 | 572.158 | 1,528 / 3,307 |
| 28 | M, A, C | 56.072 | 79.685 | 8.694 | 144.453 | 157.617 | 1,519 / 3,271 |
| 29 | R, M, A, C | 436.498 | 80.025 | 12.582 | 529.106 | 542.309 | 1,522 / 3,281 |
| 30 | D, M, A, C | 55.640 | 117.873 | 8.725 | 182.239 | 195.415 | 1,519 / 3,273 |
| 31 | R, D, M, A, C | 435.987 | 118.245 | 12.682 | 566.915 | 580.117 | 1,522 / 3,283 |

## Current-branch full-matrix comparison

The two historical matrices above were compared with fresh-process runs of the
current branch for all 32 masks in both final-transfer modes. Each current
configuration was run once with search profiling enabled and the default
15-worker capacity. All 64 runs passed validation, and every itinerary and
node count matched its corresponding historical result. The capture mask 00
wrapper did not record its exit-code field, but its output contains both
`valid=1` and the full-profile validation-success marker.

| ID | Search: old → current (s) | Change | Calculation total: old → current (s) | Change |
|---:|---:|---:|---:|---:|
| **Fly-By final** |||||
| 00 | 38.975 → 29.556 | −24.2% | 58.267 → 31.420 | −46.1% |
| 01 | 411.932 → 359.677 | −12.7% | 439.183 → 362.181 | −17.5% |
| 02 | 36.790 → 30.151 | −18.0% | 65.897 → 32.954 | −50.0% |
| 03 | 410.981 → 361.458 | −12.0% | 447.962 → 364.808 | −18.6% |
| 04 | 38.168 → 32.427 | −15.0% | 62.541 → 34.954 | −44.1% |
| 05 | 396.269 → 359.102 | −9.4% | 427.842 → 362.038 | −15.4% |
| 06 | 37.635 → 31.263 | −16.9% | 76.115 → 34.872 | −54.2% |
| 07 | 397.025 → 352.919 | −11.1% | 442.569 → 357.021 | −19.3% |
| 08 | 49.546 → 44.639 | −9.9% | 130.814 → 46.620 | −64.4% |
| 09 | 407.152 → 364.743 | −10.4% | 496.225 → 367.244 | −26.0% |
| 10 | 49.576 → 44.704 | −9.8% | 140.254 → 47.501 | −66.1% |
| 11 | 406.251 → 364.884 | −10.2% | 504.891 → 368.230 | −27.1% |
| 12 | 51.776 → 46.795 | −9.6% | 138.114 → 49.413 | −64.2% |
| 13 | 408.921 → 367.789 | −10.1% | 502.362 → 370.772 | −26.2% |
| 14 | 51.654 → 46.816 | −9.4% | 152.315 → 50.614 | −66.8% |
| 15 | 408.867 → 367.818 | −10.0% | 516.341 → 371.988 | −28.0% |
| 16 | 36.717 → 29.927 | −18.5% | 59.415 → 31.961 | −46.2% |
| 17 | 393.873 → 352.274 | −10.6% | 424.026 → 354.849 | −16.3% |
| 18 | 36.588 → 29.802 | −18.5% | 70.276 → 32.750 | −53.4% |
| 19 | 393.865 → 350.200 | −11.1% | 434.947 → 353.699 | −18.7% |
| 20 | 38.429 → 32.005 | −16.7% | 63.548 → 34.191 | −46.2% |
| 21 | 395.497 → 353.264 | −10.7% | 428.018 → 356.026 | −16.8% |
| 22 | 38.162 → 32.012 | −16.1% | 78.822 → 35.492 | −55.0% |
| 23 | 396.601 → 353.173 | −11.0% | 444.717 → 357.249 | −19.7% |
| 24 | 50.302 → 45.387 | −9.8% | 117.832 → 47.564 | −59.6% |
| 25 | 406.832 → 365.620 | −10.1% | 481.909 → 368.260 | −23.6% |
| 26 | 50.372 → 45.466 | −9.7% | 128.787 → 48.543 | −62.3% |
| 27 | 408.131 → 365.571 | −10.4% | 494.058 → 369.132 | −25.3% |
| 28 | 52.453 → 47.538 | −9.4% | 122.264 → 49.889 | −59.2% |
| 29 | 409.640 → 368.070 | −10.1% | 486.974 → 370.911 | −23.8% |
| 30 | 52.341 → 47.559 | −9.1% | 137.751 → 51.208 | −62.8% |
| 31 | 409.432 → 369.491 | −9.8% | 502.207 → 373.607 | −25.6% |
| **Capture final** |||||
| 00 | 39.604 → 29.711 | −25.0% | 55.743 → 31.488 | −43.5% |
| 01 | 418.522 → 349.261 | −16.5% | 440.662 → 351.330 | −20.3% |
| 02 | 40.171 → 29.218 | −27.3% | 65.936 → 31.802 | −51.8% |
| 03 | 419.822 → 349.468 | −16.8% | 449.442 → 352.388 | −21.6% |
| 04 | 41.341 → 31.310 | −24.3% | 60.355 → 33.346 | −44.8% |
| 05 | 422.008 → 352.170 | −16.5% | 444.793 → 354.538 | −20.3% |
| 06 | 41.048 → 31.329 | −23.7% | 74.164 → 34.566 | −53.4% |
| 07 | 420.974 → 352.719 | −16.2% | 457.817 → 356.385 | −22.2% |
| 08 | 53.236 → 44.730 | −16.0% | 146.997 → 51.471 | −65.0% |
| 09 | 436.376 → 365.184 | −16.3% | 525.853 → 372.317 | −29.2% |
| 10 | 53.279 → 44.751 | −16.0% | 171.523 → 54.236 | −68.4% |
| 11 | 435.802 → 365.493 | −16.1% | 558.011 → 375.338 | −32.7% |
| 12 | 55.583 → 46.860 | −15.7% | 142.956 → 53.809 | −62.4% |
| 13 | 437.546 → 368.159 | −15.9% | 528.645 → 375.374 | −29.0% |
| 14 | 55.176 → 46.956 | −14.9% | 179.030 → 56.965 | −68.2% |
| 15 | 437.705 → 368.064 | −15.9% | 565.413 → 378.377 | −33.1% |
| 16 | 40.313 → 29.932 | −25.8% | 60.958 → 31.928 | −47.6% |
| 17 | 422.820 → 349.734 | −17.3% | 447.326 → 352.060 | −21.3% |
| 18 | 40.344 → 30.047 | −25.5% | 72.047 → 33.014 | −54.2% |
| 19 | 421.936 → 350.416 | −17.0% | 457.405 → 353.704 | −22.7% |
| 20 | 42.256 → 32.311 | −23.5% | 65.695 → 34.604 | −47.3% |
| 21 | 423.741 → 353.196 | −16.6% | 450.919 → 355.846 | −21.1% |
| 22 | 42.007 → 32.247 | −23.2% | 81.133 → 35.923 | −55.7% |
| 23 | 424.352 → 353.911 | −16.6% | 466.961 → 358.050 | −23.3% |
| 24 | 53.454 → 45.655 | −14.6% | 139.928 → 52.513 | −62.5% |
| 25 | 436.157 → 366.028 | −16.1% | 527.138 → 373.291 | −29.2% |
| 26 | 53.721 → 45.594 | −15.1% | 174.515 → 55.331 | −68.3% |
| 27 | 433.809 → 366.006 | −15.6% | 558.907 → 376.170 | −32.7% |
| 28 | 56.072 → 47.873 | −14.6% | 144.453 → 54.958 | −62.0% |
| 29 | 436.498 → 370.058 | −15.2% | 529.106 → 377.449 | −28.7% |
| 30 | 55.640 → 47.941 | −13.8% | 182.239 → 58.232 | −68.0% |
| 31 | 435.987 → 369.112 | −15.3% | 566.915 → 379.759 | −33.0% |

Across Fly-By final, search time summed across the matrix fell from 7,170.753
to 6,392.100 s (10.9%); mean search time fell from 224.086 to 199.753 s.
Calculation total fell from 9,077.243 to 6,487.961 s (28.5%); its mean fell
from 283.664 to 202.749 s. Across Capture final, search time fell from
7,627.300 to 6,365.444 s (16.5%), with mean falling from 238.353 to 198.920
s. Calculation total fell from 9,792.985 to 6,546.562 s (33.2%), with mean
falling from 306.031 to 204.580 s.

These are end-to-end comparisons between different implementation points.
The current branch includes other changes since the historical matrices,
including parallel refinement and route optimization; therefore the full
matrix improvements must not be attributed solely to the atomic cancellation
read. The configuration-00 before/after experiment below is the more focused
measurement of that change. As with the original tables, each configuration
has one accepted sample, not repeated medians.

## Option averages

Each off/on mean averages 16 configurations, with the other four options
balanced across their on/off states.

| Option | Flyby off → on (change) | Capture off → on (change) |
|---|---:|---:|
| Resonant flybys | 100.19 → 467.14 s (+366.3%) | 113.605 → 498.457 s (+338.8%) |
| DSM | 277.46 → 289.87 s (+4.5%) | 294.470 → 317.591 s (+7.9%) |
| Moon-aware flybys | 280.92 → 286.41 s (+2.0%) | 303.274 → 308.787 s (+1.8%) |
| Moon assist on arrival | 251.51 → 315.82 s (+25.6%) | 259.460 → 352.602 s (+35.9%) |
| Custom starting orbit | 287.61 → 279.72 s (−2.7%) | 304.209 → 307.853 s (+1.2%) |

| Final transfer | Sum of calculation times | Sum of outer-process times | Mean calculation time |
|---|---:|---:|---:|
| Flyby | 9,077.243 s | 9,116.778 s | 283.664 s |
| Capture | 9,792.985 s | 10,016.413 s | 306.031 s |

## Worker-time averages

These are summed worker-seconds per configuration, not wall-clock seconds.

| Component | Flyby mean | Capture mean |
|---|---:|---:|
| Initial grid | 2.432 | 2.472 |
| Recursive search | 3,335.049 | 3,550.239 |
| End-node collection | 0.044 | 0.053 |
| Delta-v pruning | 158.895 | 175.147 |
| Flyby search | 3,173.395 | 3,371.623 |
| Route expansion/other | 2.715 | 3.417 |

| Flyby body | Flyby worker s/run | Capture worker s/run |
|---|---:|---:|
| Earth | 1,058.609 | 1,126.649 |
| Mars | 1,842.252 | 1,953.951 |
| Jupiter | 171.966 | 183.586 |
| Saturn | 100.568 | 107.438 |

## Selected-candidate refinement parallelization

To measure the refinement worker pool, the same `RelWithDebInfo` build and
Fly-By profile configuration 00 were run twice sequentially and twice with
parallel refinement. Configuration 00 has no resonant flybys, DSM, moon-aware
flybys, moon assist, or custom starting orbit. Each fresh-process run passed
the full-profile validation and produced 9,233 itineraries and 18,261 nodes.
The route counts and best refined delta-v were identical across the runs.

| Execution | Refinement run times (s) | Mean refinement (s) | Total calculation times (s) | Mean total (s) |
|---|---:|---:|---:|---:|
| Sequential | 10.78, 10.77 | 10.775 | 55.622, 55.948 | 55.785 |
| Parallel | 0.99, 0.98 | 0.985 | 45.371, 45.408 | 45.390 |

For this scenario, parallel refinement was **10.9x faster** (about 90.9% less
refinement time). Overall calculation time fell by about **18.6%** (about
1.23x). These are two samples per mode on one machine, so treat them as an
indicative scenario measurement rather than a general performance guarantee.
Global candidate selection and merging remain serial.

## Route scoring parallelization

The same `RelWithDebInfo` build and Fly-By profile configuration 00 were run
twice with serial scoring and twice with parallel scoring. Each run passed
full-profile validation and produced the same 9,233 itineraries, 18,261 nodes,
7,857 valid scored routes, and best initial/refined delta-v (4,723.640 ->
4,675.650 m/s).

| Execution | Scoring + candidate merge + repair run times (s) | Mean combined time (s) | Total calculation times (s) | Mean total (s) |
|---|---:|---:|---:|---:|
| Sequential | 6.95, 6.97 | 6.96 | 46.285, 46.412 | 46.349 |
| Parallel | 2.77, 2.77 | 2.77 | 42.738, 42.783 | 42.761 |

At the time of this comparison, the optimizer's exact-scoring timer included
candidate merging and the following repair stage, so these values measure
**scoring, candidate merge, and repair**, not scoring alone. The combined stage
was about **2.5x faster** (about 60.2% less time), and overall calculation time
fell by about **7.7%**. Separate scoring, ranking/merge, challenge-selection,
and repair timings are reported by the current build. These are two samples per
mode on one machine, so the results are indicative rather than a general
performance guarantee. Candidate selection and merging remain serial and
deterministic.

## Concurrent Fly-By and Capture optimization

The UI-free Earth-to-Saturn scenario was run twice through
`search_for_itineraries_both_modes` using `KMATSearchIntegration.exe
--full-profile-both-config 0`, before and after enabling concurrent mode
optimization. Both modes passed full-profile validation and produced the same
route metrics: 7,857 valid Fly-By scores, a best Fly-By refined delta-v of
4,675.650 m/s, 9,233 Fly-By itineraries, and 3,529 nodes / 1,632 itineraries
for Capture.

| Execution | Total calculation times (s) | Mean total (s) |
|---|---:|---:|
| Sequential modes | 45.591, 45.695 | 45.643 |
| Concurrent modes | 45.005, 45.309 | 45.157 |

Concurrency reduced total time by about **0.49 s (1.1%)** in this scenario.
The gain is modest because Capture optimization is a smaller portion of this
workload and the available worker budget is divided between the two modes.
These are two samples per mode on one machine, not a general performance
guarantee.

## Rejected-route repair parallelization

Repair timing was separated from exact scoring, then the same both-modes
configuration 00 was run twice with serial repair attempts and twice with
parallel repair attempts. Fly-By and Capture optimization were concurrent in
both comparisons; only the per-mode repair attempts changed.

| Measurement | Serial repair | Parallel repair |
|---|---:|---:|
| Fly-By repair time (s, mean) | 2.82, 2.82 (2.82) | 0.48, 0.48 (0.48) |
| Capture repair time (s, mean) | 2.16, 2.18 (2.17) | 0.43, 0.42 (0.43) |
| Total calculation time (s, mean) | 45.199, 44.471 (44.835) | 42.321, 42.698 (42.510) |

Repair time fell by about **5.9x for Fly-By** and **5.1x for Capture**. Overall
calculation time fell by about **5.2%**. Both parallel runs passed full-profile
validation and matched on valid route counts, best Fly-By refined delta-v,
itinerary counts, and node counts.

Each repair slot now receives its own deterministic random stream, assigned in
slot order before workers start. This makes repeated parallel runs independent
of worker scheduling, but changes the random sequence from the previous shared
serial stream; in this sample the Capture improved-route count changed from 527
to 526. Treat the performance and route-selection comparison as scenario
measurements, not a guarantee that every repaired candidate is identical.

## Global ranking and challenge-selection timing

The both-modes configuration 00 profile was run twice with the current timing
instrumentation. Both runs passed full-profile validation. Top-candidate
ranking/merge measures the sorted top-route insertion and replacement work;
challenge selection measures reservoir sampling and is excluded from the
ranking/merge figure. Route scoring covers the scoring workers. Repair timing
still includes repair attempts, verification, and the insertion of recovered
routes, so these stage timings are not all additive.

| Mode | Route scoring (s, mean) | Top-candidate ranking/merge (s, mean) | Challenge selection (ms, mean) | Repair (s, mean) |
|---|---:|---:|---:|---:|
| Fly-By | 0.84, 0.84 (0.84) | 0.0009, 0.0009 (0.0009) | 3.256, 3.098 (3.177) | 0.48, 0.48 (0.48) |
| Capture | 0.97, 0.97 (0.97) | 0.0002, 0.0002 (0.0002) | 0.577, 0.569 (0.573) | 0.43, 0.43 (0.43) |

Ranking/merge took about **0.9 ms for Fly-By** and **0.2 ms for Capture**;
challenge selection took about **3.2 ms** and **0.6 ms**, respectively.
Both costs are negligible beside route scoring, repair, and refinement in this
scenario, so further parallelizing ranking or challenge selection is not
recommended.

## Departure-search worker scaling

The single-mode Fly-By profile configuration 00 was run once at each worker
limit using `KMATSearchIntegration.exe --full-profile-worker-scale <workers> 0`.
The Windows host reports 8 physical cores and 16 logical processors; KMAT
reserves one logical processor, so its effective maximum is 15 workers. The
search-phase profiler was enabled for all runs. Every run passed full-profile
validation and produced the same 9,233 itineraries and 18,261 nodes.

| Worker limit | Search (s) | Optimization/refinement (s) | Filtering (s) | Calculation total (s) |
|---:|---:|---:|---:|---:|
| 1 | 369.647 | 18.062 | 0.383 | 388.094 |
| 2 | 186.514 | 8.912 | 0.204 | 195.631 |
| 4 | 95.851 | 4.696 | 0.204 | 100.752 |
| 8 | 54.807 | 2.884 | 0.226 | 57.918 |
| 12 | 39.248 | 1.987 | 0.224 | 41.459 |
| 15 | 38.633 | 1.702 | 0.224 | 40.560 |

Search was about **9.6x faster at 15 workers than at 1**. Gains tapered off
near the available worker cap: 15 workers reduced search time by only about
**1.6% compared with 12**. This suggests the default 15-worker cap is already
near the useful limit for this workload; it does not support raising the cap
above the machine's logical-processor budget. These are single samples per
limit, so repeat runs would be needed to distinguish small differences from
system noise.

## Atomic cancellation-read experiment

The 15-worker, single-mode Fly-By configuration 00 was profiled before and
after changing cancellation-counter reads from the shared mutex to an atomic
read. The pre-change baseline is one sample; the post-change result is the
mean of three fresh-process runs. All runs passed full-profile validation with
9,233 itineraries and 18,261 nodes.

| Version | Search time (s) | Calculation total (s) |
|---|---:|---:|
| Mutex read, before change | 38.633 | 40.560 |
| Atomic read, after change (mean of 3) | 30.168 | 32.108 |

Search time fell by about **21.9%**, and total calculation time by about
**20.8%** in this scenario. The atomic change preserves the existing
cancellation counter API: writes remain synchronized and publish the same
counter value to lock-free readers. This result supports the shared
cancellation lock as a significant source of contention, though repeated
pre-change samples would make the comparison stronger.

## Interpretation notes

- The all-off capture baseline was slightly faster than the flyby baseline;
  across the full matrix, capture averaged about 7.9% slower. Transfer type
  changes route feasibility and optimization workload, so capture is not
  uniformly slower.
- The capture matrix uses `TF_CAPTURE`; the flyby matrix uses `TF_FLYBY`.
  Capture includes arrival capture delta-v in the limits. The custom orbit
  changes route feasibility/counts, so its time delta is not purely overhead.
- Outer-process time includes post-search validation and should not be
  confused with calculation wall time. For moon-assist capture runs, the
  validation pass calculates assisted capture costs.
- One accepted timing sample per configuration means small differences may be
  affected by machine load and run order. The results are useful for comparing
  this scenario, but are not repeated-run performance estimates.


## CPU speedup: Kepler solver for orbit propagation

`propagate_orbit_time` (orbitlib `src/orbit.c`) found the true anomaly with a
step-halving search to a 1 s tolerance. `osv_from_elements` cost about
2,860 ns per call, while a Lambert solve costs about 100 ns, so target-state
propagation dominated every flyby evaluation. Elliptic orbits (`0 <= e < 1`)
now use a Newton solve of Kepler's equation (about 509 ns per call, 5.6x
faster); the old search remains as a fallback and for `e >= 1`.

| Case | Before | After |
|---|---|---|
| Coverage run, total wall time | 88.7 s | 47.7 s |
| Jupiter, budget 512 | 5.6 s | 3.6 s |
| Jupiter, budget 2,048 | 14.8 s | 10.7 s |
| Saturn, budget 512 | 28.1 s | 11.8 s |

Node and itinerary counts moved by at most about 0.2%, because the old
propagator was only accurate to about 1 s of orbital time. Integration checks
that were calibrated to that numerical noise (DSM total comparison, resonant
re-solve cost for maneuver-free routes, and the DSM model date pair) were
adjusted.


## CPU speedup: SOI iteration freeze and golden-section reuse

Profiling at budget 512 showed the per-seed powered flyby optimization
(`calculate_powered_flyby`, about 4.5 us per call) as the other large cost next to
the root-finder, and it grows with the seed budget.

- **Golden-section reuse** in `calculate_powered_flyby` (orbitlib): each of the 32
  refinement steps now evaluates one new periapsis radius instead of two.
- **SOI iteration freeze** in `powered_route_objective.c`: the periapsis radius
  and side flipped between near-equal optima, so many routes oscillated until the
  60-iteration limit (4,544 solves in one integration run). Freezing the choice
  after four iterations cuts that to 84, and routes that previously failed the
  convergence check now converge. The "zero-delta-v DSM equals the plain route"
  check passes on every date pair again.

| Case | After Kepler solver | After both changes |
|---|---|---|
| Jupiter, budget 512 | 3.6 s | 2.7 s |
| Jupiter, budget 2,048 | 10.7 s | 8.2 s |
| Saturn, budget 512 | 11.8 s | 9.8 s |
| Coverage run, total wall time | 47.7 s | 37.9 s |

Itinerary counts are unchanged at budget 512 (4,598 Jupiter, 8,908 Saturn). The
integration test no longer requires rejected-route repairs, because the search
fixture no longer produces rejections.
