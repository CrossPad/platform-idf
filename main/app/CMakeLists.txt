# ============================================================================
# App Module Manager - Automatic Discovery
# ============================================================================
# This file automatically discovers and includes all applications.
# No manual changes needed when adding new apps!
#
# To add a new app:
# 1. Create directory: main/app/your_app/
# 2. Add your source files: your_app.cpp, etc.
# 3. Create main/app/your_app/CMakeLists.txt with:
#    set(YOUR_APP_SOURCES
#        "app/your_app/your_app.cpp"
#        "app/your_app/other_file.cpp"
#    )
# 4. Use REGISTER_APP macro in your source
# 5. That's it! CMake will auto-discover it on next build.
# ============================================================================

set(APP_SOURCES "")

# Optional template app — excluded by default. Enable with:
#   idf.py -DCROSSPAD_INCLUDE_EXAMPLE_APP=ON fullclean build
option(CROSSPAD_INCLUDE_EXAMPLE_APP "Include the 'example' template app in the build" OFF)

# Auto-discover all app subdirectories with CMakeLists.txt
file(GLOB APP_CMAKE_FILES "${CMAKE_CURRENT_LIST_DIR}/*/CMakeLists.txt")

foreach(app_cmake ${APP_CMAKE_FILES})
    # Get directory name (app name)
    get_filename_component(app_dir ${app_cmake} DIRECTORY)
    get_filename_component(app_name ${app_dir} NAME)

    if(app_name STREQUAL "example" AND NOT CROSSPAD_INCLUDE_EXAMPLE_APP)
        message(STATUS "Skipping app 'example' (enable with -DCROSSPAD_INCLUDE_EXAMPLE_APP=ON)")
        continue()
    endif()

    message(STATUS "Discovered app: ${app_name}")

    # Include the app's CMakeLists.txt
    include(${app_cmake})

    # The app's CMakeLists.txt should define ${APP_NAME}_SOURCES variable
    # We need to find it dynamically
    string(TOUPPER ${app_name} APP_NAME_UPPER)

    # Try common variable naming patterns
    if(DEFINED ${APP_NAME_UPPER}_SOURCES)
        list(APPEND APP_SOURCES ${${APP_NAME_UPPER}_SOURCES})
    elseif(DEFINED ${app_name}_SOURCES)
        list(APPEND APP_SOURCES ${${app_name}_SOURCES})
    else()
        # Fallback: scan for any *_SOURCES variable defined by this file
        get_directory_property(ALL_VARS VARIABLES)
        foreach(var ${ALL_VARS})
            if(var MATCHES ".*_SOURCES$" AND NOT var STREQUAL "APP_SOURCES")
                list(APPEND APP_SOURCES ${${var}})
            endif()
        endforeach()
    endif()
endforeach()

# Auto-generate app_registry_init.cpp with calls to _register_*_app() functions
# created by REGISTER_APP macros. Static constructors are NOT reliable on ESP-IDF.
# Scanned: the in-tree apps above, then every installed app component's src/.
include(${CMAKE_CURRENT_LIST_DIR}/../../components/crosspad-core/cmake/crosspad_app_registry.cmake)
# A single-app kit (CROSSPAD_FW_STANDALONE, from its profile) registers only
# its own apps, none of the in-tree ones. An app whose crosspad-app.json names
# "kits" belongs to those kits only: elsewhere its component is empty (DOOM).
set(_registry_kit "full")
if(DEFINED CROSSPAD_FW_KIT)
    set(_registry_kit "${CROSSPAD_FW_KIT}")
endif()
set(_registry_files "")
if(NOT CROSSPAD_FW_STANDALONE)
    foreach(_src ${APP_SOURCES})
        list(APPEND _registry_files "${CMAKE_CURRENT_LIST_DIR}/../${_src}")
    endforeach()
endif()
set(_registry_dirs "")
file(GLOB _registry_components "${CMAKE_SOURCE_DIR}/components/crosspad-*/")
foreach(_comp_dir ${_registry_components})
    get_filename_component(_comp_name "${_comp_dir}" NAME)
    if(_comp_name MATCHES "^crosspad-(core|gui|platform-idf)$")
        continue()
    endif()
    string(REGEX REPLACE "^crosspad-" "" _comp_id "${_comp_name}")
    if(CROSSPAD_FW_STANDALONE AND NOT _comp_id IN_LIST CROSSPAD_FW_KIT_APPS)
        continue()
    endif()
    if(EXISTS "${_comp_dir}/crosspad-app.json")
        file(READ "${_comp_dir}/crosspad-app.json" _comp_json)
        string(JSON _kits_n ERROR_VARIABLE _kits_err LENGTH "${_comp_json}" kits)
        if(NOT _kits_err)
            set(_in_kit FALSE)
            math(EXPR _kits_last "${_kits_n} - 1")
            foreach(_i RANGE ${_kits_last})
                string(JSON _k GET "${_comp_json}" kits ${_i})
                if(_k STREQUAL _registry_kit)
                    set(_in_kit TRUE)
                endif()
            endforeach()
            if(NOT _in_kit)
                continue()
            endif()
        endif()
    endif()
    list(APPEND _registry_dirs "${_comp_dir}/src")
endforeach()
crosspad_generate_app_registry(
    OUTPUT "${CMAKE_CURRENT_LIST_DIR}/app_registry_init.cpp"
    FILES  ${_registry_files}
    DIRS   ${_registry_dirs})
list(APPEND APP_SOURCES "app/app_registry_init.cpp")

# This file is include()d (not add_subdirectory()d) by main/CMakeLists.txt,
# so it already runs in that scope — APP_SOURCES is visible there as-is.
# PARENT_SCOPE here would leak it one level further, into the top-level
# CMakeLists.txt, which is exactly what apps are told never to do.

message(STATUS "Total app sources collected: ${APP_SOURCES}")
