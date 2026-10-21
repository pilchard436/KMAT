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
