# ============================================================================
# Asset Collector - Aggregates images from multiple sources into a staging dir
# ============================================================================
# Runs at CMake CONFIGURE time. Creates a flat staging directory that
# littlefs_create_partition_image() uses to build the assets partition.
#
# Priority order (first wins on filename collision):
#   1. crosspad-gui assets  (components/crosspad-gui/assets/)
#   2. App assets            (main/app/*/assets/)
#   3. Project-level images  (main/images/data/)
#
# IMPORTANT: Like app discovery, adding new asset files requires
# `idf.py reconfigure` since file(GLOB) runs at configure time only.
# ============================================================================

set(ASSET_STAGING_DIR "${CMAKE_BINARY_DIR}/assets_staging")

# Clean and recreate staging directory
file(REMOVE_RECURSE "${ASSET_STAGING_DIR}")
file(MAKE_DIRECTORY "${ASSET_STAGING_DIR}")

set(STAGED_FILES "")

# ── 1. crosspad-gui assets (highest priority) ───────────────────────────────
set(GUI_ASSETS_DIR "${CMAKE_SOURCE_DIR}/components/crosspad-gui/assets")
if(EXISTS "${GUI_ASSETS_DIR}")
    file(GLOB GUI_ASSET_FILES "${GUI_ASSETS_DIR}/*.png")
    foreach(f ${GUI_ASSET_FILES})
        get_filename_component(fname ${f} NAME)
        file(COPY_FILE ${f} "${ASSET_STAGING_DIR}/${fname}")
        list(APPEND STAGED_FILES "${fname}")
    endforeach()
    list(LENGTH GUI_ASSET_FILES _gui_count)
    message(STATUS "Assets: staged ${_gui_count} GUI assets from crosspad-gui")
else()
    message(STATUS "Assets: no crosspad-gui/assets/ directory found")
endif()

# ── 2. App assets (auto-discovered) ─────────────────────────────────────────
file(GLOB APP_ASSET_DIRS "${CMAKE_SOURCE_DIR}/main/app/*/assets")
foreach(app_asset_dir ${APP_ASSET_DIRS})
    get_filename_component(app_parent ${app_asset_dir} DIRECTORY)
    get_filename_component(app_name ${app_parent} NAME)

    file(GLOB APP_ASSET_FILES "${app_asset_dir}/*.png")
    foreach(f ${APP_ASSET_FILES})
        get_filename_component(fname ${f} NAME)
        if("${fname}" IN_LIST STAGED_FILES)
            message(WARNING "Asset collision: '${fname}' from app '${app_name}' overwrites a previously staged file!")
        endif()
        file(COPY_FILE ${f} "${ASSET_STAGING_DIR}/${fname}")
        list(APPEND STAGED_FILES "${fname}")
    endforeach()
    list(LENGTH APP_ASSET_FILES _app_count)
    message(STATUS "Assets: staged ${_app_count} assets from app '${app_name}'")
endforeach()

# ── 2b. Installed app components (components/crosspad-*/assets) ────────────
# Apps installed by the app manager are components, not main/app/ directories,
# so their icons were never staged — an app that shipped its own icon showed a
# blank tile in the launcher unless the file happened to exist elsewhere.
file(GLOB COMPONENT_ASSET_DIRS "${CMAKE_SOURCE_DIR}/components/crosspad-*/assets")
# crosspad-gui is staged above as the shared set; re-staging it here would
# report every one of its files as a collision with itself.
list(REMOVE_ITEM COMPONENT_ASSET_DIRS
     "${CMAKE_SOURCE_DIR}/components/crosspad-gui/assets"
     "${CMAKE_SOURCE_DIR}/components/crosspad-core/assets")
foreach(comp_asset_dir ${COMPONENT_ASSET_DIRS})
    get_filename_component(comp_parent ${comp_asset_dir} DIRECTORY)
    get_filename_component(comp_name ${comp_parent} NAME)

    file(GLOB COMP_ASSET_FILES "${comp_asset_dir}/*.png")
    foreach(f ${COMP_ASSET_FILES})
        get_filename_component(fname ${f} NAME)
        if("${fname}" IN_LIST STAGED_FILES)
            message(WARNING "Asset collision: '${fname}' from component "
                            "'${comp_name}' overwrites a previously staged file!")
        endif()
        file(COPY_FILE ${f} "${ASSET_STAGING_DIR}/${fname}")
        list(APPEND STAGED_FILES "${fname}")
    endforeach()
    list(LENGTH COMP_ASSET_FILES _comp_count)
    if(_comp_count GREATER 0)
        message(STATUS "Assets: staged ${_comp_count} assets from component '${comp_name}'")
    endif()
endforeach()

# ── 3. Project-level images (lowest priority, no overwrite) ─────────────────
set(PROJECT_IMAGES_DIR "${CMAKE_SOURCE_DIR}/main/images/data")
if(EXISTS "${PROJECT_IMAGES_DIR}")
    file(GLOB PROJECT_ASSET_FILES "${PROJECT_IMAGES_DIR}/*.png" "${PROJECT_IMAGES_DIR}/*.bin")
    set(_proj_staged 0)
    set(_proj_skipped 0)
    foreach(f ${PROJECT_ASSET_FILES})
        get_filename_component(fname ${f} NAME)
        if(NOT "${fname}" IN_LIST STAGED_FILES)
            file(COPY_FILE ${f} "${ASSET_STAGING_DIR}/${fname}")
            list(APPEND STAGED_FILES "${fname}")
            math(EXPR _proj_staged "${_proj_staged} + 1")
        else()
            math(EXPR _proj_skipped "${_proj_skipped} + 1")
        endif()
    endforeach()
    message(STATUS "Assets: staged ${_proj_staged} project images (${_proj_skipped} skipped as duplicates)")
endif()

# ── 3b. Bundled STM32 firmware image ───────────────────────────────────────
# main/stm32_fw/stm32_fw.bin is the CrossPad_STM32_r2 build this firmware was
# written against; stm32_fw_update_auto() flashes it into the STM at boot when
# the running STM is older or speaks another protocol. Refresh it by copying
# CrossPad_STM32_r2/build/<preset>/CrossPad_STM32_r20.bin here.
set(STM_FW_IMAGE "${CMAKE_SOURCE_DIR}/main/stm32_fw/stm32_fw.bin")
if(EXISTS "${STM_FW_IMAGE}")
    file(COPY_FILE "${STM_FW_IMAGE}" "${ASSET_STAGING_DIR}/stm32_fw.bin")
    file(SIZE "${STM_FW_IMAGE}" _stm_fw_size)
    message(STATUS "Assets: staged STM32 firmware image (${_stm_fw_size} B)")
else()
    message(STATUS "Assets: no main/stm32_fw/stm32_fw.bin — STM auto-update disabled")
endif()

# ── 4. Documentation files (markdown) ──────────────────────────────────────
set(DOCS_DIR "${CMAKE_SOURCE_DIR}/main/docs")
if(EXISTS "${DOCS_DIR}")
    file(MAKE_DIRECTORY "${ASSET_STAGING_DIR}/docs")
    file(GLOB DOCS_FILES "${DOCS_DIR}/*.md")
    foreach(f ${DOCS_FILES})
        get_filename_component(fname ${f} NAME)
        file(COPY_FILE ${f} "${ASSET_STAGING_DIR}/docs/${fname}")
    endforeach()
    list(LENGTH DOCS_FILES _docs_count)
    math(EXPR TOTAL_EXTRA "${TOTAL_EXTRA} + ${_docs_count}" OUTPUT_FORMAT DECIMAL)
    message(STATUS "Assets: staged ${_docs_count} docs from main/docs/")
endif()

# ── Summary ──────────────────────────────────────────────────────────────────
list(LENGTH STAGED_FILES TOTAL_ASSET_COUNT)
message(STATUS "Assets: total ${TOTAL_ASSET_COUNT} files + docs staged in ${ASSET_STAGING_DIR}")

# ── 5. Assets id ─────────────────────────────────────────────────────────────
# Identity of this asset set: the image must carry its own marker, so the id is
# a hash of the staged files rather than of assets.bin. Recursive so nested
# files (main/docs/*.md staged under docs/) are counted; relative paths in the
# digest so a nested file can't collide with a top-level one of the same name.
file(GLOB_RECURSE _staged_assets LIST_DIRECTORIES false "${ASSET_STAGING_DIR}/*")
list(SORT _staged_assets)
set(_assets_digest "")
foreach(f ${_staged_assets})
    file(RELATIVE_PATH _n "${ASSET_STAGING_DIR}" ${f})
    file(SHA256 ${f} _h)
    string(APPEND _assets_digest "${_n}:${_h}\n")
endforeach()
string(SHA256 ASSETS_ID "${_assets_digest}")
file(WRITE "${ASSET_STAGING_DIR}/.assets_id" "${ASSETS_ID}")
file(WRITE "${CMAKE_BINARY_DIR}/assets_id.txt" "${ASSETS_ID}")

string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _assets_id_bytes "${ASSETS_ID}")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
file(WRITE "${CMAKE_BINARY_DIR}/generated/assets_id.h"
     "#pragma once\n#define FW_ASSETS_ID_BYTES { ${_assets_id_bytes} }\n")
