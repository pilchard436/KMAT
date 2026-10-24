cmake_minimum_required(VERSION 3.26)
project(KMAT C)
include(FetchContent)
FetchContent_Declare(nanopb
    GIT_REPOSITORY https://github.com/nanopb/nanopb.git
    GIT_TAG 0.4.9.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nanopb)
FetchContent_Declare(krpc_cnano
    URL https://github.com/krpc/krpc/releases/download/v0.6.0/krpc-cnano-0.6.0.zip
    URL_HASH SHA256=9FAE249B97D156C4BCA5CB239C79156D73C8261F596F9CBAF9D830ACABA6AD97
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _kmat_fetch_only
)
FetchContent_MakeAvailable(krpc_cnano)
set(KMAT_KRPC_WIRE_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/kmat_krpc_wire.c")
if(NOT EXISTS "${KMAT_KRPC_WIRE_SOURCE}")
    file(DOWNLOAD
        https://raw.githubusercontent.com/krpc/krpc/0fc0c74439ae609a3794d7f54cfe4510dea3d620/client/cnano/src/krpc.c
        "${KMAT_KRPC_WIRE_SOURCE}"
        EXPECTED_HASH SHA256=8a7127c5f2e4b9b359eecff33c8f0e355738558a5aff2d1cbda27581ba9a0d6d
        TLS_VERIFY ON
        STATUS KMAT_KRPC_WIRE_DOWNLOAD_STATUS
    )
    list(GET KMAT_KRPC_WIRE_DOWNLOAD_STATUS 0 KMAT_KRPC_WIRE_DOWNLOAD_CODE)
    if(NOT KMAT_KRPC_WIRE_DOWNLOAD_CODE EQUAL 0)
        message(FATAL_ERROR "Could not download the pinned Cnano TCP framing source: ${KMAT_KRPC_WIRE_DOWNLOAD_STATUS}")
    endif()
endif()
file(SHA256 "${KMAT_KRPC_WIRE_SOURCE}" KMAT_KRPC_WIRE_ACTUAL_HASH)
if(NOT KMAT_KRPC_WIRE_ACTUAL_HASH STREQUAL "8a7127c5f2e4b9b359eecff33c8f0e355738558a5aff2d1cbda27581ba9a0d6d")
    message(FATAL_ERROR "The pinned Cnano TCP framing source has an unexpected hash")
endif()
set(KMAT_KRPC_ROOT "${krpc_cnano_SOURCE_DIR}")
if(NOT EXISTS "${KMAT_KRPC_ROOT}/src/krpc.c")
    file(GLOB KMAT_KRPC_CHILDREN LIST_DIRECTORIES TRUE "${KMAT_KRPC_ROOT}/*")
    list(GET KMAT_KRPC_CHILDREN 0 KMAT_KRPC_ROOT)
endif()
file(GLOB KMAT_NANOPB_HEADERS CONFIGURE_DEPENDS "${nanopb_SOURCE_DIR}/*.h")
file(COPY ${KMAT_NANOPB_HEADERS} DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/kmat_nanopb/krpc_cnano")
file(GLOB KMAT_KRPC_SOURCES CONFIGURE_DEPENDS "${KMAT_KRPC_ROOT}/src/*.c")
list(FILTER KMAT_KRPC_SOURCES EXCLUDE REGEX "/communication\\.c$")
list(FILTER KMAT_KRPC_SOURCES EXCLUDE REGEX "/krpc\\.c$")
list(APPEND KMAT_KRPC_SOURCES "${KMAT_KRPC_WIRE_SOURCE}")
add_library(kmat_krpc_cnano STATIC ${KMAT_KRPC_SOURCES} ksp/ksp_transport.c)
target_include_directories(kmat_krpc_cnano PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/ksp"
    "${KMAT_KRPC_ROOT}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/kmat_nanopb"
)
target_compile_definitions(kmat_krpc_cnano PUBLIC KRPC_ERROR_MESSAGES KRPC_COMMUNICATION_CUSTOM KRPC_SINGLE_CONNECTION KRPC_BUFFER_SIZE=1024)
if(MSVC)
    target_compile_options(kmat_krpc_cnano PUBLIC "/FI${CMAKE_CURRENT_SOURCE_DIR}/ksp/krpc_types.h")
else()
    target_compile_options(kmat_krpc_cnano PUBLIC -include "${CMAKE_CURRENT_SOURCE_DIR}/ksp/krpc_types.h")
endif()
target_link_libraries(kmat_krpc_cnano PUBLIC protobuf-nanopb-static)
if(WIN32)
    target_link_libraries(kmat_krpc_cnano PUBLIC ws2_32)
endif()

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
include(CTest)
option(KMAT_ENABLE_SEARCH_PROFILE "Enable detailed per-body itinerary search profiling" OFF)

if(MSVC)
    add_compile_options($<$<CONFIG:Release>:/O2>)
elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options($<$<CONFIG:Release>:-O3>)
endif()

include_directories(.)

add_subdirectory(external/orbitlib)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(external/orbitlib/src/transfer.c PROPERTIES
        COMPILE_OPTIONS "$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-ffast-math>"
    )
elseif(MSVC)
    set_source_files_properties(external/orbitlib/src/transfer.c PROPERTIES
        COMPILE_OPTIONS "$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/fp:fast>"
    )
endif()

set(KMAT_FILE_IO_SOURCES
    tools/file_io.c
    tools/file_io_system.c
    tools/file_io_steps.c
    tools/file_io_itins.c
    tools/file_io_itin.c
)

set(KMAT_ITINERARY_SOURCES
    orbit_calculator/itin_tool.c
    orbit_calculator/search_rejections.c
    orbit_calculator/itinerary_tree.c
    orbit_calculator/itinerary_text.c
    orbit_calculator/flyby_policy.c
    orbit_calculator/moon_flyby.c
    orbit_calculator/soi_encounter.c
    orbit_calculator/flyby_search.c
    orbit_calculator/leg_variants.c
    orbit_calculator/resonant_flybys.c
)

add_executable(KMAT
    ${KMAT_FILE_IO_SOURCES}
    tools/file_io_format.c
    tools/itinerary_date_limits.c
    tools/itinerary_date_input.c
    tools/itinerary_date_picker_state.c
    tools/itinerary_dv_limit.c
    tools/itinerary_reference_defaults.c
    tools/itinerary_reference_defaults.h
    tools/starting_orbit.c
    tools/starting_orbit.h
    tools/file_io.h
    launch_calculator/launch_calculator.c
    launch_calculator/launch_calculator.h
    launch_calculator/lv_profile.c
    launch_calculator/lv_profile.h
    main.c
    ksp/ksp_rpc.c
    ksp/ksp_session.c
    ksp/ksp_nodes.c
    ksp/ksp_node_refinement.c
    ksp/ksp_node_correction.c
    tools/crash_log.c
    tools/tool_funcs.c
    tools/tool_funcs.h
    launch_calculator/launch_circularization.c
    launch_calculator/launch_circularization.h
    launch_calculator/lp_parameters.c
    launch_calculator/lp_parameters.h
    orbit_calculator/transfer_calc.c
    orbit_calculator/transfer_search_worker.c
    orbit_calculator/transfer_search_progress.c
    orbit_calculator/transfer_calc.h
    orbit_calculator/powered_flyby_optimizer.c
    orbit_calculator/powered_route_objective.c
    orbit_calculator/powered_route_dsm.c
    orbit_calculator/powered_route_candidates.c
    orbit_calculator/powered_flyby_optimizer.h
    launch_calculator/lp_parameters.h
    tools/thread_pool.c
    tools/thread_pool.h
    gui/drawing_3d.c
    gui/drawing_2d.c
    gui/drawing_primitives.c
    gui/drawing_plots.c
    gui/drawing.h
    tools/thread_pool.h
    database/database.c
    database/database.h
    ${KMAT_ITINERARY_SOURCES}
    orbit_calculator/itin_tool.h
    gui/transfer_app/transfer_planner.c
    gui/transfer_app/transfer_planner_view.c
    gui/transfer_app/transfer_planner_panel.c
    gui/transfer_app/transfer_planner_io.c
    gui/transfer_app/transfer_planner.h
    gui/transfer_app/porkchop_analyzer.c
    gui/transfer_app/porkchop_analyzer_groups.c
    gui/transfer_app/porkchop_analyzer_filter.c
    gui/transfer_app/porkchop_analyzer_io.c
    gui/transfer_app/porkchop_analyzer.h
    gui/transfer_app/porkchop_analyzer_model.c
    gui/transfer_app/porkchop_analyzer_model.h
    gui/transfer_app/porkchop_analyzer_tools.c
    gui/transfer_app/porkchop_analyzer_tools.h
    gui/transfer_app/sequence_calculator.c
    gui/transfer_app/sequence_calculator.h
    launch_calculator/launch_sim.c
    launch_calculator/launch_sim.h
    launch_calculator/launch_state.c
    launch_calculator/launch_state.h
    gui/launch_app/launch_analyzer.c
    gui/launch_app/launch_analyzer.h
    database/lv_database.c
    database/lv_database.h
    gui/launch_app/capability_analyzer.c
    gui/launch_app/capability_analyzer.h
    launch_calculator/capability_calculator.c
    launch_calculator/capability_calculator.h
    gui/launch_app/launch_parameter_analyzer.c
    gui/launch_app/launch_parameter_analyzer.h
    gui/database_app/mission_db.c
    gui/database_app/mission_db.h
    database/mission_database.c
    database/mission_database.h
    gui/css_loader.c
    gui/css_loader.h
    gui/database_app/database_app_tools/mission_db_tools.c
    gui/database_app/database_app_tools/mission_db_tools.h
    gui/database_app/database_app_tools/mission_manager_tools.c
    gui/database_app/database_app_tools/mission_manager_objectives.c
    gui/database_app/database_app_tools/mission_manager_events.c
    gui/database_app/database_app_tools/mission_manager_tools.h
    gui/transfer_app/itinerary_calculator.c
    gui/transfer_app/itinerary_calculator_dates.c
    gui/transfer_app/date_picker.c
    gui/transfer_app/itinerary_calculator_run.c
    gui/transfer_app/itinerary_calculator.h
    gui/transfer_app/date_picker.h
    gui/transfer_app/starting_orbit_editor.c
    gui/transfer_app/starting_orbit_editor.h
    gui/gui_manager.c
    gui/gui_manager.h
    gui/info_win_manager.c
    gui/info_win_manager.h
    gui/settings.c
    gui/settings.h
    00_Testing/testing.c
    00_Testing/testing.h
    00_Testing/testing_algorithms.c
    00_Testing/testing_algorithms.h
    00_Testing/test_support.c
    00_Testing/unit_tests.c
    00_Testing/unit_tests_math.c
    00_Testing/unit_tests_lambert.c
    00_Testing/unit_tests_powered_flyby.c
    00_Testing/unit_tests_starting_orbit.c
    00_Testing/unit_tests_formats.c
    00_Testing/unit_tests_itinerary_date_input.c
    00_Testing/unit_tests_porkchop.c
    00_Testing/unit_tests_reference_defaults.c
    00_Testing/unit_tests_itinerary.c
    00_Testing/unit_tests_moon_flyby.c
    00_Testing/unit_tests_optimizer.c
    00_Testing/unit_tests_ksp.c
    gui/gui_tools/camera.c
    gui/gui_tools/camera.h
    gui/gui_tools/screen.c
    gui/gui_tools/screen.h
    tools/version_tool.c
    tools/version_tool.h
    tools/celestial_systems.c
)
target_link_libraries(KMAT kmat_krpc_cnano)

# The unit suites are compiled into KMAT and run after every build of it.
option(KMAT_RUN_UNIT_TESTS "Run the unit tests after building KMAT" ON)
if(KMAT_RUN_UNIT_TESTS)
    add_custom_command(TARGET KMAT POST_BUILD
        COMMAND KMAT --unit-tests
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Running unit tests"
        USES_TERMINAL
    )
endif()

add_executable(KMATSearchIntegration
    00_Testing/itinerary_search_integration.c
    00_Testing/integration_search.c
    00_Testing/integration_starting_orbit.c
    00_Testing/integration_benchmarks.c
    00_Testing/integration_operations.c
    00_Testing/integration_flybys.c
    00_Testing/integration_coverage.c
    orbit_calculator/transfer_calc.c
    orbit_calculator/transfer_search_worker.c
    orbit_calculator/transfer_search_progress.c
    orbit_calculator/powered_flyby_optimizer.c
    orbit_calculator/powered_route_objective.c
    orbit_calculator/powered_route_dsm.c
    orbit_calculator/powered_route_candidates.c
    ${KMAT_ITINERARY_SOURCES}
    tools/starting_orbit.c
    ${KMAT_FILE_IO_SOURCES}
    tools/file_io_format.c
    tools/thread_pool.c
    tools/tool_funcs.c
)
target_link_libraries(KMATSearchIntegration PRIVATE orbitlib geometrylib)
target_include_directories(KMATSearchIntegration PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/external/orbitlib/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/orbitlib/external/geometrylib/include
)

if(KMAT_ENABLE_SEARCH_PROFILE)
    target_compile_definitions(KMAT PRIVATE KMAT_ENABLE_SEARCH_PROFILE)
    target_compile_definitions(KMATSearchIntegration PRIVATE KMAT_ENABLE_SEARCH_PROFILE)
endif()

# Platform-specific setup
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        message(STATUS "Configuring Cross-Compilation for Windows (MinGW-w64)")
        set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
        set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)

        # Ensure MinGW headers are used
        include_directories(/usr/x86_64-w64-mingw32/include)
        link_directories(/usr/x86_64-w64-mingw32/lib)

        # GTK3 installation directory (modify if needed)
        set(GTK3_ROOT "/usr/x86_64-w64-mingw32/gtk" CACHE PATH "GTK3 installation directory")
    else()
        message(STATUS "Configuring for Windows (native MinGW)")
        set(GTK3_ROOT "C:/msys64/mingw64" CACHE PATH "GTK3 installation directory")
    endif ()

    include_directories(
        ${GTK3_ROOT}/include
        ${GTK3_ROOT}/include/gtk-3.0
        ${GTK3_ROOT}/include/glib-2.0
        ${GTK3_ROOT}/lib/glib-2.0/include
        ${GTK3_ROOT}/include/pango-1.0
        ${GTK3_ROOT}/include/cairo
        ${GTK3_ROOT}/include/gdk-pixbuf-2.0
        ${GTK3_ROOT}/include/atk-1.0
        ${GTK3_ROOT}/include/harfbuzz
    )

    # Explicit GTK3 library directories
    link_directories(
        ${GTK3_ROOT}/lib
        ${GTK3_ROOT}/lib/gtk-3.0
        ${GTK3_ROOT}/lib/glib-2.0
    )

    target_link_libraries(KMAT
        orbitlib
        gtk-3
        gdk-3
        gobject-2.0
        glib-2.0
        pango-1.0
        cairo
        gdk_pixbuf-2.0
        atk-1.0
        gio-2.0
        urlmon
        sqlite3
        wininet
    )

    target_link_libraries(KMATSearchIntegration PRIVATE urlmon glib-2.0 gobject-2.0)

    target_include_directories(KMAT PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/external/orbitlib/include
    )


    set_target_properties(KMAT PROPERTIES WIN32_EXECUTABLE FALSE)

else(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "Configuring for Linux (GCC)")

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)

    include_directories(${GTK3_INCLUDE_DIRS})
    link_directories(${GTK3_LIBRARY_DIRS})
    add_definitions(${GTK3_CFLAGS_OTHER})
    target_link_libraries(KMAT m ${GTK3_LIBRARIES} sqlite3 orbitlib)
    target_link_libraries(KMATSearchIntegration PRIVATE ${GTK3_LIBRARIES})

    target_link_options(KMAT PRIVATE -rdynamic)

    target_include_directories(KMAT PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/external/orbitlib/include
    )

endif()

if(BUILD_TESTING)
    add_test(NAME KMATIntegrationTests COMMAND KMAT --test)
    add_test(NAME KMATSearchIntegration COMMAND KMATSearchIntegration)
    add_test(NAME KMATUnitTests COMMAND KMAT --unit-tests)
    set_tests_properties(KMATIntegrationTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        LABELS integration
    )
    set_tests_properties(KMATUnitTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        LABELS unit
    )
    set_tests_properties(KMATSearchIntegration PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        LABELS integration
    )

    add_custom_target(run_integration_tests
        COMMAND ${CMAKE_CTEST_COMMAND} --verbose --output-on-failure -L integration
        DEPENDS KMAT KMATSearchIntegration
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        USES_TERMINAL
    )
endif()
