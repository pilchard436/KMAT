# Porkchop Analyzer: exploring itinerary results

The Porkchop Analyzer displays itinerary results from a loaded `.itins` file,
or from a search sent with **Show in Porkchop Analyzer** in the Itinerary
Calculator.
The plot shows departure date against either transfer duration or arrival date;
point color represents total delta-v.

## Explore the plot

- **Left-drag** a rectangle to zoom into that departure-date and
  duration/arrival-date range.
- **Click a plotted node** to select its itinerary. The selected node is
  outlined and its route appears in the preview.
- **Focus the plot and use the arrow keys** to select a nearby node in that
  direction. Navigation prefers nodes more aligned with the arrow's direction,
  then falls back to the nearest off-axis node if needed.
- **Right-drag** to pan the view without changing the filters.
- **Scroll** to zoom around the pointer.
- **Hover near a plotted point** to see its delta-v, departure date, arrival
  date, and transfer duration. Away from a node, the tooltip reports the active
  y-axis value at the cursor.

Filter changes apply as soon as a complete value is entered. Date fields accept
the active date format and include a calendar picker; invalid or incomplete
dates and numeric values are highlighted and revert to their last valid value
when focus leaves the field. Calendar arrow keys step across month and year
boundaries; ordinal calendars also roll from the last day of one year to day
one of the next. Switching the y-axis between duration and arrival date shows
the matching filter row; filters in the hidden row are inactive. Switching
axes preserves the current plot zoom and pan.
Switching between Fly-By and Capture also preserves the current plot view.
**Save Itineraries** saves all shown results, Fly-By and Capture, as an
`.itins` file. **Save Selected Itinerary** saves the currently selected route.
**Open in Transfer Planner** opens the selected route in the Transfer Planner
and switches to it, with the same search settings and delta-v values a saved
and reloaded route would have.
**Select Best in View** selects the lowest-delta-v node in the current plot
window, considering only results included by the filters and visible itinerary
groups. Applying a filter resets the plot view to the remaining results.
When **Capture** is selected, the moon-assist checkbox is available only for
itinerary files calculated with arrival moon assist enabled. These files retain
both assisted and unassisted arrival delta-v values, so changing the option
updates the plot and filters without recalculation. The selected route's
per-step delta-v preview also uses maneuver values saved in the itinerary file.
The route preview lists each leg and its duration in the top-right corner, as
the Transfer Planner does, labels each planet with its name, and draws planet
orbits faintly so the route stands out.
**Reset Porkchop** restores the full result set and its default view.
