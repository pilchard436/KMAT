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
}
