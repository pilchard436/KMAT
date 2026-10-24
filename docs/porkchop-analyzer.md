# Porkchop Analyzer: exploring itinerary results

The Porkchop Analyzer displays itinerary results from a loaded `.itins` file.
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
**Save Selected Itinerary** saves the currently selected route.
**Select Best in View** selects the lowest-delta-v node in the current plot
window, considering only results included by the filters and visible itinerary
groups. Applying a filter resets the plot view to the remaining results.
When **Capture** is selected, the checkbox below it controls whether available
arrival moon-assist savings are included in Capture delta-v. Changing the option
recalculates capture costs and reapplies the current filters.
**Reset Porkchop** restores the full result set and its default view.
