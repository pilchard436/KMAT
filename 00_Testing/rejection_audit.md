# Rejected-route timing-repair audit

Run from `build` after building `KMATSearchIntegration`:

```powershell
./KMATSearchIntegration.exe --audit-rejections
./KMATSearchIntegration.exe --full-profile --audit-rejections
```

The audit is disabled by default. It does not insert repaired candidates into the
normal results. It samples two disjoint populations of complete, initialized
multi-body routes reaching the powered optimizer:

1. Initial SOI/objective evaluation failures, before timing refinement.
2. Initially valid routes omitted from the final top-500 refinement set, including
   routes displaced as better candidates arrive.

Each population uses Algorithm R reservoir sampling with a fixed PRNG seed,
retaining at most 32 route copies. Sampling is over route candidates, not unique
body sequences. All members are tested when a population has fewer than 32 routes.

Each sampled route receives up to 48 local/global timing probes, followed by
coordinate refinement from its best feasible dates. The total is bounded at 192
objective evaluations per sample, including a final independent evaluation.
Departure date, bodies, physical model, and user limits stay fixed. All subsequent
encounter dates can move on the private copy, including dates in a shared prefix
that normal refinement holds fixed. This provides more timing freedom and work
than the normal 96-evaluation refinement; it measures recoverability under that
larger allowance.

## Measured results

Full-year Earth-to-Saturn ephemeris fixture, 366 departure dates:

| Population | Population size | Sampled | Feasible after repair | Improved from initial valid cost | Beat worst retained corrected route | Beat best retained corrected route |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Initial evaluation failed | 601 | 32 | 27 | N/A | 0 | 0 |
| Excluded by refinement cap | 2745 | 32 | 32 | 32 | 14 | 0 |

The retained corrected objective range was 4712.108921 to 6687.340203 m/s.
The final full-profile run passed in 58.76 seconds, including 2.635 seconds and
5183 objective evaluations for the audit. Normal returned results remained 6763
nodes and 3346 itineraries, with the same best corrected objective as the unaudited
fixture. Unit, normal search, and rejection-audit CTest targets all passed.
The best recovered initial failure cost 6778.735403 m/s. The best audited cutoff
route cost 6547.166766 m/s. Witness body sequences and Julian dates are printed
in the audit log so their objectives can be reproduced with the fixture settings.

In the small Earth-Venus-Jupiter fixture, all six initial failures were audited
and became feasible; four beat the worst retained corrected route, none beat the
best. There were no valid candidates excluded by the refinement cap in that case.

These observations show recoverable routes are rejected at the initial gate,
and some excluded candidates can outperform retained candidates. They do not
establish a probability of missing the global optimum. The measured 27/32 and
14/32 fractions describe these samples under this repair policy.

## Coverage and validation

Production now applies a smaller version of this repair (see `TODO.md`): shared-prefix
dates stay fixed, 144 evaluations per route, 64 stratified samples. The audit remains
the wider-freedom measurement. Full-year production result: 29 of 34 sampled
failures recovered, 12 of 32 challengers beat the worst ranked route, none beat the
best (4712.109 m/s).

The audit does not observe routes removed before the powered optimizer, unsampled
encounter dates, candidates omitted by the 512-seed budget, or DSM-dependent
trajectories. It does not distinguish physical infeasibility from numerical
nonconvergence within the initial-failure population. Failure to repair a sample
is not proof of physical infeasibility.

The `KMATRejectionAudit` CTest checks recovery on the small fixture, reservoir
sample sizes, finite recovered objectives, and evaluation budgets. Existing
normal search and objective-grid regressions run alongside it. The full-profile
audit also checks returned route constraints and sample/work-budget invariants.
