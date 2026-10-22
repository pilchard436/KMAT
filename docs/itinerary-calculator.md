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
focus leaves the field. The date range validation and date-entry behavior are
implemented in
[`itinerary_calculator_dates.c`](../GUI/transfer_app/itinerary_calculator_dates.c).

## 2. The UI choices become search data

`prepare_ic_calc_data_from_widgets()` copies the form values into the
calculator's search configuration:

- **System and bodies:** the selected system/central body, departure body,
  arrival body, and allowed intermediate flyby bodies.
- **Departure window:** the earliest and latest departure dates are converted
  to Julian dates. The search uses one-day increments through this range.
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

- **Allow resonant flybys:** considers returns to the same body after one or
  more revolutions, such as an Earth-to-Earth leg. This expands the search and
  can make it several times slower.
- **Allow deep-space maneuvers:** enables an additional optimization pass to
  search for a maneuver between bodies on selected routes. DSM delta-v counts
  toward the satellite delta-v limit; a zero satellite limit therefore
  excludes routes that need a DSM.
- **Moon-aware flybys:** checks for flybys that hit a moon or pass too close to
  one. It can reject or re-plan routes and makes searching slower.
- **Allow moon assist on arrival:** checks whether a moon flyby can reduce the
  arrival capture burn when one is available.
- **Custom starting orbit:** when enabled and valid for the selected departure
  body, it is used to calculate the departure burn instead of the usual
  departure-orbit assumption.

These flags are read from the form at the start of the calculation. If an
optional control is unavailable, it is treated as disabled.

New `.itins` search files and `.itin` route files store the search options
alongside their results. When a saved route is reopened, those settings are
restored for option-dependent calculations and displays. Older files do not
contain this provenance, so those settings cannot be recovered from them.

## 4. The search runs in the background

After preparing the inputs, the GUI disables the Itinerary Calculator window,
initializes progress tracking, and starts a worker thread. A progress window
reports search progress while the worker performs the calculation.

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
through powered-flyby optimization unless the search has been cancelled. This
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
on arrival, and custom starting orbit, respectively. Running each mask in a
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
files marked for a single mode remain readable and keep that mode fixed.
The existing single-itinerary `.itin` format remains unchanged and does not
record the mode. A route loaded directly into Transfer Planner therefore has
no known source mode and remains selectable; changing that mode updates the
route's displayed arrival objective without rerunning the itinerary search.
