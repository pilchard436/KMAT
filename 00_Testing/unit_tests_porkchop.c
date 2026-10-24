#include "unit_tests.h"

void run_porkchop_tests(void) {
    const double pi = 3.14159265358979323846;
    const double tolerance = 1e-12;
    (void)pi;
    (void)tolerance;
    struct PorkchopGroup hidden_group = {.show_group = 0};
    struct PorkchopGroup visible_group = {.show_group = 1};
    struct PorkchopAnalyzerPoint porkchop_points[] = {
        {.inside_filter = 0, .group = &visible_group},
        {.inside_filter = 1, .group = &hidden_group},
        {.inside_filter = 1, .group = &visible_group}
    };
    expect_int("visible porkchop selection skips filtered and hidden points",
                           find_first_visible_porkchop_point(porkchop_points, 3), 2);
    porkchop_points[2].inside_filter = 0;
    expect_int("visible porkchop selection returns no-match sentinel",
                           find_first_visible_porkchop_point(porkchop_points, 3), -1);

    Body earth = {.name = "Earth"};
    Body mars = {.name = "Mars"};
    Body venus = {.name = "Venus"};
    Body jupiter = {.name = "Jupiter"};
    Body *system_bodies[] = {&earth, &mars, &venus, &jupiter};
    CelestSystem system = {.num_bodies = 4, .bodies = system_bodies};
    struct ItinStep departure = {.body = &earth};
    struct ItinStep flyby = {.body = &venus, .prev = &departure};
    struct ItinStep arrival = {.body = &mars, .prev = &flyby};
    int body_visibility[] = {0, 0, 0, 1};
    mark_itinerary_bodies_visible(body_visibility, 4, &arrival, &system);
    expect_int("route marks departure body visible", body_visibility[0], 1);
    expect_int("route marks flyby body visible", body_visibility[2], 1);
    expect_int("route marks arrival body visible", body_visibility[1], 1);
    expect_int("route leaves unrelated body hidden", body_visibility[3], 0);


    struct PorkchopGroup group_list[] = {
        {.has_itin_inside_filter = 1, .show_group = 1},
        {.has_itin_inside_filter = 0, .show_group = 1},
        {.has_itin_inside_filter = 1, .show_group = 0}
    };
    struct PorkchopAnalyzerPoint group_points[] = {
        {.inside_filter = 1, .group = &group_list[2]},
        {.inside_filter = 1, .group = &group_list[0]}
    };
    expect_int("only groups with a filtered itinerary change when hiding all",
               set_porkchop_groups_visibility(group_list, 3, 0), 1);
    expect_int("hide all hides the groups inside the filter", group_list[0].show_group == 0 && group_list[2].show_group == 0, 1);
    expect_int("hide all leaves a group outside the filter alone", group_list[1].show_group, 1);
    expect_int("nothing is visible after hide all", find_first_visible_porkchop_point(group_points, 2), -1);
    expect_int("show all reports the groups it changed", set_porkchop_groups_visibility(group_list, 3, 1), 2);
    expect_int("the first visible point follows the shown groups", find_first_visible_porkchop_point(group_points, 2), 0);
    group_list[2].show_group = 0;
    expect_int("a single group toggle leaves the others shown", group_list[0].show_group, 1);
    expect_int("the first visible point skips the hidden group", find_first_visible_porkchop_point(group_points, 2), 1);
    // Toggling must not reorder or reallocate the list, or the rebuilt rows would reset the scroll position.
    struct PorkchopGroup *list_before = group_list;
    set_porkchop_groups_visibility(group_list, 3, 0);
    set_porkchop_groups_visibility(group_list, 3, 1);
    expect_int("toggling groups keeps the list and its point links in place", list_before == group_list && group_points[0].group == &group_list[2] && group_points[1].group == &group_list[0], 1);
    expect_int("a missing group list changes nothing", set_porkchop_groups_visibility(NULL, 3, 1), 0);

    struct PorkchopPlotRange plot_range = {20, 80, 20, 80};
    struct PorkchopPlotRange plot_bounds = {0, 100, 0, 100};
    expect_int("porkchop plot range accepts increasing finite bounds", porkchop_plot_range_is_valid(&plot_range), 1);
    porkchop_plot_pan(&plot_range, &plot_bounds, 0.5, -1.0);
    expect_int("porkchop plot pan stays inside full data bounds",
               plot_range.min_x == 0 && plot_range.max_x == 60 &&
               plot_range.min_y == 0 && plot_range.max_y == 60, 1);

    plot_range = (struct PorkchopPlotRange) {0, 100, 10, 110};
    struct PorkchopPlotRange zoom_bounds = {0, 200, 0, 200};
    porkchop_plot_zoom(&plot_range, &zoom_bounds, 0.5, 0.25, 0.75);
    expect_int("porkchop plot zoom keeps its cursor anchor fixed",
               plot_range.min_x == 12.5 && plot_range.max_x == 62.5 &&
               plot_range.min_y == 47.5 && plot_range.max_y == 97.5, 1);
    double x_value = 0, y_value = 0;
    expect_int("porkchop plot converts cursor fractions to data values",
               porkchop_plot_value_at_fraction(&plot_range, 0.25, 0.75, &x_value, &y_value) &&
               x_value == 25 && y_value == 85, 1);
    expect_int("porkchop plot rejects a degenerate range",
               porkchop_plot_value_at_fraction(&(struct PorkchopPlotRange) {1, 1, 0, 1}, 0.5, 0.5, &x_value, &y_value), 0);
    plot_range = (struct PorkchopPlotRange) {20, 80, 0, 60};
    porkchop_plot_zoom(&plot_range, &plot_bounds, 2.0, 0.5, 0.0);
    expect_int("zooming out at the duration boundary stays within data bounds",
               plot_range.min_x == 0 && plot_range.max_x == 100 &&
               plot_range.min_y == 0 && plot_range.max_y == 100, 1);
    plot_range = (struct PorkchopPlotRange) {0, 100, 0, 100};
    expect_int("box zoom selects the chosen data rectangle",
               porkchop_plot_zoom_to_box(&plot_range, 0.2, 0.6, 0.1, 0.7) &&
               plot_range.min_x == 20 && plot_range.max_x == 60 &&
               plot_range.min_y == 10 && plot_range.max_y == 70, 1);
    porkchop_plot_pan(&plot_range, &plot_bounds, -0.5, -0.5);
    expect_int("panning after box zoom can move beyond the selected rectangle",
               plot_range.min_x == 40 && plot_range.max_x == 80 &&
               plot_range.min_y == 0 && plot_range.max_y == 60, 1);

    struct PorkchopPoint dv_point = {
        .dv_dep = 100,
        .dv_dsm = 20,
        .dv_flyby = 30,
        .dv_arr_cap = 40,
        .dv_arr_circ = 50
    };
    expect_int("flyby delta-v excludes arrival capture and circularization",
               porkchop_point_total_dv(dv_point, TF_FLYBY) == 150, 1);
    expect_int("capture delta-v includes arrival capture",
               porkchop_point_total_dv(dv_point, TF_CAPTURE) == 190, 1);
    expect_int("circularization delta-v includes arrival circularization",
               porkchop_point_total_dv(dv_point, TF_CIRC) == 200, 1);
}
