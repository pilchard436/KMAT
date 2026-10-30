# Itinerary Calculator: what happens when you click Calculate

This document describes the current flow of the **Calculate** button in KMAT's
Itinerary Calculator, from input validation through route search and optional
saving.

## 1. The click is validated

The button's GTK `clicked` signal calls `on_calc_ic()` in
[`itinerary_calculator_run.c`](../GUI/transfer_app/itinerary_calculator_run.c).
The handler:

1. Returns immediately if no celestial system is selected.
2. Validates the departure and arrival date constraints. The latest departure
   must not precede the earliest departure. When the maximum-arrival-date mode
   is selected, the latest arrival must also be on or after the latest
   departure. An invalid range prevents the search from starting.
3. Prepares a custom starting orbit if that option is enabled. The orbit must
   have been created for the currently selected departure body and its values
   must be valid. Otherwise an error message is shown and the search stops.
   When the option is disabled, any active custom orbit is cleared.

The date entry fields also validate input while editing and normalize it when
focus leaves the field. Their calendar picker supports arrow-key navigation
across month and year boundaries. For ordinal calendars, stepping past the
last day advances the year, and stepping back from day one goes to the
previous year's last day. The date range validation and date-entry behavior
are implemented in
[`itinerary_calculator_dates.c`](../GUI/transfer_app/itinerary_calculator_dates.c).

## 2. The UI choices become search data

`prepare_ic_calc_data_from_widgets()` copies the form values into the
calculator's search configuration:

- **System and bodies:** the selected system/central body, departure body,
  arrival body, and allowed intermediate flyby bodies.
- **Departure window:** the earliest and latest departure dates are converted
  to Julian dates. The search advances by one day in the selected calendar:
  one Earth day for ISO dates, and one Kerbin day (six hours, or 0.25 Earth
  days) for Kerbin dates.
- **Arrival/time limit:** the radio selection determines which limit is used:
  - With **maximum arrival date**, that date is the arrival cutoff. The maximum
    duration is derived from the earliest departure date to that cutoff.
  - With **maximum duration**, the entered duration is used to determine the
    arrival cutoff from the latest departure date. Kerbal-time durations are
    divided by four when converted to Julian days.
- **KSP snapshot date (optional):** when a KSP start snapshot is active for
  the selected departure body, its date replaces the earliest departure date.
  If the latest departure date is earlier than the snapshot, it is moved up to
  the snapshot date. The user-selected latest departure date otherwise remains
  in effect.
- **Delta-v limits:** blank maximum departure or satellite delta-v fields mean
  no limit for that category. The total delta-v cap is the sum of the two
  limits. Departure burn is checked against the departure limit; subsequent
  satellite costs are checked against the satellite limit.
- **Fly-By and Capture results:** both are computed in one search. The shared
  route search uses the looser Fly-By arrival filter, then Capture routes are
  separately optimized and filtered with their capture cost. Porkchop Analyzer
  can switch between the two saved result sets without rerunning the search.
- **Periapsis:** the calculator uses the configured departure and arrival
  periapsis altitudes, offset by each body's atmospheric altitude, for burn
  calculations.

The arrival body is always added to the permitted-body list for the search.
Other selected bodies in the flyby-body grid can be visited en route. The grid
defaults to bodies between the departure and arrival bodies, with Ceres and
Vesta and Dres initially excluded; the endpoints themselves are not editable
as intermediate-body checkboxes.

## 3. Optional search settings

The following checkboxes alter the search or route evaluation:

- **Allow powered flybys** (on by default): allows flybys that need a burn at
  the flyby body and optimizes the best routes (dates, flyby burns,
  sphere-of-influence corrections, rejected-route repair). Turning it off
  searches only unpowered gravity assists, as earlier KMAT versions did: no
  burn-requiring flybys are added and the optimization stage is skipped. On the
  Earth-to-Saturn benchmark that takes the search from 7.8 s to 3.5 s (2.3×)
  and the all-options search from 62.7 s to 38.6 s (1.6×), with about 1% fewer
  routes and none with optimized dates or burns. Deep-space maneuvers are
  optimized in the same stage, so
  turning this off also unchecks and disables **Allow deep-space maneuvers**.
- **Allow resonant flybys:** considers returns to the same body after one or
  more revolutions, such as an Earth-to-Earth leg. This expands the search and
  can make it several times slower.
- **Allow deep-space maneuvers:** enables an additional optimization pass to
  search for a maneuver between bodies on selected routes. DSM delta-v counts
  toward the satellite delta-v limit; a zero satellite limit therefore
  excludes routes that need a DSM. It requires powered flybys.
- **Moon-aware flybys:** checks for flybys that hit a moon or pass too close to
  one. It can reject or re-plan routes and makes searching slower.
- **Allow moon assist on arrival:** checks whether a moon flyby can reduce the
  arrival capture burn when one is available.
- **Custom starting orbit:** when enabled and valid for the selected departure
  body, it is used to calculate the departure burn instead of the usual
  departure-orbit assumption.

These flags are read from the form at the start of the calculation. If an
optional control is unavailable, it is treated as disabled.

### Search depth

The **Settings** tab has an **Itinerary search depth (seeds per departure)**
dropdown (512, 1024, 2048 by default, 4096, 8192) with an info icon whose
tooltip explains the setting. It sets how many powered
flyby seeds each departure may add before the search stops adding them. When
many options are enabled, a low value can exhaust the budget before the search
reaches some route families; a higher value finds more of them but takes
longer. The choice is saved as `powered-seed-budget` in the settings file and
applies to searches started afterwards. See
[`itinerary-cpu-speedup.md`](./itinerary-cpu-speedup.md#raising-the-seed-budget)
for measurements.

The budget is shared out as the search descends the route tree. At each step the
candidate flybys are grouped by body, and a body's subtree may use only about
half of the seeds still available when later bodies are waiting, so an early,
expensive family (for example Eve) can no longer starve a later one (for
example Duna). Unused seeds flow on to the next body. On the Earth to Saturn
benchmark this gave 9,300 instead of 9,238 itineraries in 21 s instead of 20 s.

### Route limits

Two fixed limits keep the search tree from growing exponentially with the
maximum duration (defined in `orbit_calculator/flyby_policy.h`):

- **Flyby limit:** a route holds at most `MAX_FLYBYS_PER_ROUTE` (5)
  intermediate flybys. Once a route has reached the limit, only legs to the
  target body are added. Reference searches showed the cheapest routes use
  3–4 flybys; longer chains were resonant waiting loops within a few m/s of
  the best.
- **Intermediate leg horizon:** a leg to a non-target body may last at most
  the larger of 1.5 synodic periods of the two bodies and 3 Hohmann transfer
  times from the current radius (`maximum_intermediate_leg_days`). Legs to the
  target body still use the full remaining duration.

On the Kerbin to Eeloo search with all options on, these limits cut the
full-year search at the default duration from about 110 s to 33 s, and a
1250-day (5000 Kerbin day) duration from infeasible (over 700 s for 15
departure days) to 79 s for a full year, with the same best delta-v.

New `.itins` search files and `.itin` route files store the search options
alongside their results. When a saved route is reopened, those settings are
restored for option-dependent calculations and displays. Older files do not
contain this provenance, so those settings cannot be recovered from them (files
saved before **Allow powered flybys** existed read as powered flybys on, which
is how they were searched).

Dual-mode `.itins` files store the five calculated delta-v components for each
completed route: departure, DSM, flyby, capture, and circularization. Arrival
values include moon-assist results when enabled; the Porkchop Analyzer reuses
them when the route inputs and arrival periapsis match. The updated type-8
record layout is not backward-compatible with earlier type-8 `.itins` files.

New `.itin` route files store the same five delta-v components for the saved
route, along with the departure and arrival periapses and moon-assist state
used to compute them. The updated `.itin` record layout is not
backward-compatible with earlier `.itin` files.

## 4. The search runs in the background

After preparing the inputs, the GUI disables the Itinerary Calculator window,
initializes progress tracking, and starts a worker thread. A progress window
reports search progress while the worker performs the calculation. The progress
bar tracks the current phase's completed work (departure dates, scored routes,
refined routes, or filtered routes). The percentage restarts when the phase
changes; the status text names the active phase and its completed/total count.
Concurrent Fly-By and Capture optimization is identified by both mode names
rather than a completed/total count. Final result preparation is shown as an
indeterminate animation because its remaining duration cannot be measured
reliably.

For each departure date, the search:

1. Computes the departure body's position and velocity from orbital elements
   or ephemeris data, depending on the system.
2. Samples initial transfer arrival times for the permitted next bodies. The
   normal first-leg search uses 500 samples per candidate body; its transfer
   window is based on a Hohmann-transfer duration and is constrained by the
   selected time limits.
3. Solves the transfer legs with Lambert calculations and estimates departure
   and arrival costs.
4. Discards candidates that already violate the delta-v constraints.
5. Extends surviving candidates into multi-leg routes by searching for viable
   flybys, continuing toward the selected arrival body within the date and
   duration limits.

Departure dates are distributed across the search thread pool. Routes are
collected as itinerary trees for later filtering and presentation, rather
than shown individually as each candidate is generated.

## 5. Routes are optimized and filtered

For the regular to-target itinerary search, surviving routes are passed
through powered-flyby optimization unless the search has been cancelled or
**Allow powered flybys** is off, in which case the routes keep the dates and
velocities found by the search and go straight to filtering. This
stage checks and refines promising routes, including their flyby burns and
timings. It can also correct relevant sphere-of-influence issues.

Moon-aware search checks for moon hits and close passes, rejecting or re-planning
flybys that violate its clearance policy. The Transfer Planner uses the same
minimum periapsis clearance as search. Powered-flyby optimization may increase
the flyby burn to keep the actual periapsis above that minimum, even when the
unpowered turn would pass closer to the body. The Transfer Planner displays the
powered periapsis when a safe powered solution exists; if not, it falls back to
the unpowered turn estimate, which may be below the body's surface. Moving a
maneuver date is allowed when the route can still be resolved with safe powered
flybys; otherwise the edit is rejected and the previous route and date are
restored. This edit-time re-solve does not apply the search's user-defined
maximum satellite delta-v. During search, that limit applies to the aggregate
of arrival, powered-flyby, and DSM burns, rather than to each transfer step.

If deep-space maneuvers are enabled, selected routes receive an additional DSM
optimization pass. The resulting routes are then checked against the date
limits, departure/satellite/total delta-v limits, and applicable moon and
sphere-of-influence constraints. Routes that fail are pruned from their
itinerary trees. The search records rejection counts so it can explain why no
routes survived.

The main search, final route filters, and powered-route optimization are in
[`transfer_calc.c`](../orbit_calculator/transfer_calc.c),
[`transfer_search_worker.c`](../orbit_calculator/transfer_search_worker.c),
and [`powered_flyby_optimizer.c`](../orbit_calculator/powered_flyby_optimizer.c).

### Parallelizing filtering and optimization

The departure-date search and post-search filtering use worker pools. Filtering
assigns each departure itinerary tree to one worker; tree pruning stays local to
that worker, then surviving trees are compacted in their original order on the
calculation thread. Rejection counts, moon-block counts, and progress increments
are synchronized, workers apply the configured thread-local moon policies, and
the pool preserves the search cancellation counter.

- **Filtering is parallelized.** Independent departure trees are filtered
  concurrently while the final tree ordering and counts remain deterministic.
- **Route scoring is parallelized before global selection.** Each arrival
  route is initialized and scored independently in worker-owned storage.
  Workers use the calculation thread's floating-point environment and the
  configured thread-local moon policies. Scored candidates and rejected routes
  are merged on the calculation thread in their original arrival order before
  updating the shared top-route, repair, and challenge pools.
- **Selected-candidate refinement is parallelized.** Global ranking and
  challenge selection remain serial. Once the candidate list is fixed,
  candidates are refined independently in worker-owned buffers; their results
  and any DSM improvements are applied to itinerary trees in candidate order on
  the calculation thread. Workers inherit the calculation thread's floating-
  point environment because solver results otherwise differ across Windows
  threads. Objective budgets and call counts remain thread-local.
- **Rejected-route repair attempts are parallelized.** Each retained repair
  slot gets a deterministic random stream in slot order and is repaired in
  worker-owned candidate data. Repaired routes are verified and merged into
  the global ranking serially in slot order; challenge sampling also remains
  serial. Route scoring, top-candidate ranking/merge, challenge selection, and
  repair timings are reported separately.
- **Fly-By and Capture optimization run concurrently when both modes are
  requested.** Capture operates on its own copy of the departure trees, and
  each optimizer keeps its candidates and statistics separate. The two mode
  workers share a bounded CPU budget with their nested scoring/refinement
  pools, use the calculation thread's floating-point environment and their
  own thread-local moon policies, and share the existing cancellation state.
  Progress reports combined completion while both optimizers run; filtering
  remains sequential after both finish. If fewer than two workers are
  available, the modes use the sequential path.

Filtering, route scoring, rejected-route repair, selected-candidate refinement,
and Fly-By/Capture optimization are parallelized while global ranking and
result merging remain deterministic. The measured scoring, ranking/merge,
challenge-selection, repair, refinement, and both-mode results are recorded in
[`itinerary-profiling-results.md`](./itinerary-profiling-results.md). Actual
end-to-end gains depend on how much of a search is spent in the parallelized
stages.

### Cancellation and incremental refinement

Search workers check the cancellation state while searching departure dates and
building routes. Currently, powered-route optimization is started only when the
search has not been cancelled. The optimizer also checks cancellation while
processing its candidate routes, so cancellation can skip the entire
optimization phase or stop it before all candidates are refined. The final
route-pruning/filtering pass still runs on the routes that remain.

It is possible to refine and filter completed departure-date results
incrementally, but simply moving the optimizer into each date worker would
change its behavior. Departure dates are searched in parallel, while the
current optimizer ranks candidates across the complete result set and applies
shared refinement, repair, and challenge limits. Per-date optimization could
therefore change which routes receive refinement, increase the total work, and
require synchronization around shared search and optimizer state. Filtering
also needs to follow refinement for routes whose optimized burns or feasibility
may change.

A lower-risk design is to distinguish **stop searching** from **cancel
refinement**: on a search cancellation, stop producing new routes, wait for
workers to finish, then refine and filter the routes found so far using the
existing global ranking and limits. This can make cancellation take longer to
complete, especially when DSM optimization is enabled; the progress UI should
indicate that it is finishing routes already found. Incremental refinement as
dates complete is another option, but it needs a coordinator and a deliberate
policy for preserving or changing the current global candidate limits.

## 6. Completion, saving, and cleanup

When the worker returns, the GUI completion callback:

1. Finishes the custom starting-orbit search state.
2. Closes the progress window and re-enables the calculator.
3. If routes exist, opens a save chooser for an `.itins` file. Saving is
   optional; cancelling the chooser skips the save. Both route sets and the
   search configuration are written in the dual-mode file format.
4. Frees both in-memory itinerary trees and the temporary flyby-body list.
5. If neither mode has itineraries, displays **“No itineraries found!”**, with
   a rejection summary when available.

This completion and save flow is implemented in
[`itinerary_calculator_run.c`](../GUI/transfer_app/itinerary_calculator_run.c).

## 7. Search profiling

Configure with `KMAT_ENABLE_SEARCH_PROFILE=ON` to enable detailed search timing.
The integration runner accepts `--full-profile-config <0..31>` to run one
flyby-final configuration, or `--full-profile-capture-config <0..31>` to run
the same configuration with capture as the final transfer. The five mask bits
represent resonant flybys, deep-space maneuvers, moon-aware flybys, moon assist
on arrival, and custom starting orbit, respectively. Appending `--unpowered`
runs the same configuration with powered flybys off (the deep-space maneuver
bit then has no effect). Running each mask in a
separate process avoids carrying thread-local caches between configurations.
The output includes wall-clock time for initialization, search, powered-route
optimization/refinement, and route filtering. Search timings are also reported
as summed worker-seconds for the initial grid and recursive search. The
recursive breakdown separates end-node collection, delta-v pruning, flyby
search, and remaining route expansion work; per-body flyby timings and root
evaluation counts are included. Summed worker-seconds are aggregate thread
time, not wall-clock time, and should not be added to the wall-clock phase
measurements.

The completed flyby-final and capture-final 32-configuration matrices, their
phase timings, and aggregate option effects are recorded in
[`itinerary-profiling-results.md`](./itinerary-profiling-results.md).

Porkchop Analyzer reads both result sets from new dual-mode `.itins` files and
switches between their independently optimized routes. Legacy calculation
files containing only a single result mode are no longer supported; `.itins`
files must contain both Fly-By and Capture result sets.
New single-itinerary `.itin` files also retain their search options and mode.
Older `.itin` files without this provenance have no known source mode; changing
the mode in Transfer Planner updates the route's displayed arrival objective
without rerunning the itinerary search.
