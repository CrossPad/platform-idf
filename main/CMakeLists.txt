# Include app-specific CMakeLists.txt
include(${CMAKE_CURRENT_LIST_DIR}/app/CMakeLists.txt)

# Build-time inventory of installed apps and shared submodules (commit + pin),
# reported over CDC as APP_VERSIONS and listed in Settings -> Info. The
# generator lives in crosspad-core so firmware and PC simulator emit the same
# table the same way.
# Paths are relative to this file: the early component-requirements pass runs
# it in a context where CMAKE_SOURCE_DIR is not the project root.
get_filename_component(_crosspad_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include(${_crosspad_root}/components/crosspad-core/cmake/crosspad_app_versions.cmake)
# An uninitialized submodule is an empty folder: not in the image, so not in
# the inventory either (it read as a stale component and failed Check).
file(GLOB _crosspad_app_lists "${_crosspad_root}/components/crosspad-*/CMakeLists.txt")
set(_crosspad_app_dirs "")
foreach(_list ${_crosspad_app_lists})
    get_filename_component(_dir "${_list}" DIRECTORY)
    list(APPEND _crosspad_app_dirs "${_dir}")
endforeach()
list(REMOVE_ITEM _crosspad_app_dirs
     "${_crosspad_root}/components/crosspad-core"
     "${_crosspad_root}/components/crosspad-gui"
     "${_crosspad_root}/components/crosspad-platform-idf")
crosspad_generate_app_versions(
    OUTPUT   "${CMAKE_BINARY_DIR}/app_versions.cpp"
    SHARED   "${_crosspad_root}/components/crosspad-core"
             "${_crosspad_root}/components/crosspad-gui"
    APPS     ${_crosspad_app_dirs}
    MANIFEST "${_crosspad_root}/apps.json")

# Auto-discover installed app-library components (crosspad-* except core infra).
# Same rule as above: this runs in the early requirements pass too, where
# CMAKE_SOURCE_DIR is not the project root and the glob came back empty —
# main never saw the apps' include dirs, only crosspad-sampler's explicit one.
set(APP_COMPONENT_REQUIRES "")
file(GLOB _app_comp_dirs "${_crosspad_root}/components/crosspad-*/")
foreach(_dir ${_app_comp_dirs})
    get_filename_component(_name "${_dir}" NAME)
    # An uninitialized submodule is an empty folder, not a component.
    if(EXISTS "${_dir}/CMakeLists.txt" AND
       NOT _name STREQUAL "crosspad-core" AND
       NOT _name STREQUAL "crosspad-gui" AND
       NOT _name STREQUAL "crosspad-platform-idf")
        list(APPEND APP_COMPONENT_REQUIRES "${_name}")
    endif()
endforeach()
message(STATUS "App component requires: ${APP_COMPONENT_REQUIRES}")

# The prebuilt streamer and the glue only it uses (Kconfig
# CROSSPAD_SAMPLER_MARCEL, off by default: the native engine plays every kit).
# sample_player_port.cpp stays either way -- its file cache, per-task slots,
# DMA stage and pacing are what the native engine reads the card through; the
# library's hooks inside it are behind the same option.
# The requirement stays unconditional: requirements are resolved before
# Kconfig is, and the component registers nothing when the option is off.
set(MARCEL_SRCS "")
if(CONFIG_CROSSPAD_SAMPLER_MARCEL)
    set(MARCEL_SRCS "audio/audio_test_sample_ply.cpp")
endif()

if(IDF_TARGET STREQUAL "esp32s3")
    set(MAIN_S3_SRCS
        "midi_loop_test.cpp"
        "usb_audio_bridge.c"
        "usb_descriptors_audio.c"
    )
    set(MAIN_S3_REQUIRES
        stm32_hal
        perfmon
        usb_device_uac
        esp_wifi
    )
else()
    set(MAIN_S3_SRCS "")
    set(MAIN_S3_REQUIRES esp_wifi)
endif()

# idf_component_register requires INCLUDE_DIRS to exist at configure time;
# collect_assets.cmake (included below) is what actually writes assets_id.h
# into it, but that runs after registration.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")

# The twin source id (TWIN_HELLO): written now so the file exists for the registration,
# and again on every build by the target below (the sources change without a reconfigure).
get_filename_component(_twin_fw "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_twin_sid "${CMAKE_BINARY_DIR}/generated/twin_source_id.c")
execute_process(COMMAND ${CMAKE_COMMAND} -DFW=${_twin_fw} -DOUT=${_twin_sid}
                        -P ${_twin_fw}/tools/twin_source_id.cmake)

idf_component_register(
    SRCS  
        "${CMAKE_BINARY_DIR}/app_versions.cpp"
        "${CMAKE_BINARY_DIR}/generated/twin_source_id.c"
        "audio_route_control.cpp"
        "companion_sysex.cpp"
        "crosspad_deps.cpp"
        "lvgl_stats.cpp"
        "file_transfer.cpp"
        "fw_assets_sync.cpp"
        "fw_image_desc.c"
        "fw_library.cpp"
        "fw_slots.cpp"
        "fw_catalog.cpp"
        "fw_download.cpp"
        "fw_store_files.cpp"
        "fw_view_data.cpp"
        "fw_view_service.cpp"
        "fw_ui_state.cpp"
        "fw_gc.cpp"
        "fw_http.cpp"
        "fw_updater.cpp"
        "fw_verify.cpp"
        "fw_updater_verbs.cpp"
        "cp_account.cpp"
        "cp_remote.cpp"
        "cp_account_verbs.cpp"
        "audio/audio_control.cpp"
        "audio/audio_module.cpp"
        "audio/audio_task.cpp"
        "audio/engine_handoff.cpp"
        "gui/gui.cpp"
        "hil_control.cpp"
        "sd_bench.cpp"
        "sd_sector_io.cpp"
        "hil_text.cpp"
        "host_info.cpp"
        "usb_host_profile.cpp"
        "wifi_creds.cpp"
        "wifi_link.cpp"
        "wifi_ping.cpp"
        "device_serial.cpp"
        "wifi_verbs.cpp"
        "imu_midi.cpp"
        "instrument_host.cpp"
        "kit_convert_port.cpp"
        "song_sounds.cpp"
        "touch_trace.cpp"
        "twin_mirror.cpp"
        "motion_stream.cpp"
        "lv_async_safe.cpp"
        "lvgl_mem_psram.c"
        "gui/ota_overlay.cpp"
        "gui/stm_fw_overlay.cpp"
        "gui/fw_flow_overlay.cpp"
        "gui/charge_overlay.cpp"
        "gui/usb_serial_prompt.cpp"
        "gui/usb_transfer_screen.cpp"
        "gui/net_mode_screen.cpp"
        "gui/wifi_page.cpp"
        "gui/acct_pair_overlay.cpp"
        "gui/wifi_fallback.cpp"
        "main.cpp"
        "midiCallbacks.cpp"
        "midi_clock.cpp"
        "ota_cdc.cpp"
        "power_button.cpp"
        "reset_log.cpp"
        "sample_player_port.cpp"
        "usb_config_manager.cpp"
        "usb_descriptors.c"
        "usb_descriptors_transfer.c"
        "usb_transfer.cpp"
        "net_mode.cpp"
        ${APP_SOURCES}
        ${MAIN_S3_SRCS}
        ${MARCEL_SRCS}

    INCLUDE_DIRS
        "include"
        "${CMAKE_BINARY_DIR}/generated"

    PRIV_REQUIRES
        app_update
        arduino_json_file_adapter
        arduino_midi_library
        bblanchon__ArduinoJson
        crosspad-core
        crosspad-gui
        crosspad-platform-idf
        crosspad-sampler
        efuse
        esp_delta_ota
        esp_event
        esp_http_client
        mqtt
        esp_netif
        esp-tls
        esp_rom
        esp_timer
        espcoredump
        fatfs
        littlefs esp_partition
        mbedtls
        midi_esp_tinyusb_transport
        ml_sample_stream_player_lib
        nvs_flash
        tinyusb
        wear_levelling
        ${MAIN_S3_REQUIRES}
        ${APP_COMPONENT_REQUIRES}
)

# Force the linker to include all object files from this component, even when
# their symbols are not referenced externally. Required for REGISTER_APP
# static constructors (AppRegistrar) to survive linking.
set_property(TARGET ${COMPONENT_LIB} PROPERTY WHOLE_ARCHIVE ON)

add_custom_target(twin_source_id
    COMMAND ${CMAKE_COMMAND} -DFW=${_twin_fw} -DOUT=${_twin_sid} -P ${_twin_fw}/tools/twin_source_id.cmake
    BYPRODUCTS ${_twin_sid}
    COMMENT "twin source id")
add_dependencies(${COMPONENT_LIB} twin_source_id)

# s_fw_image_desc (fw_image_desc.c) is nothing but a variable in a section
# nobody calls into, so --gc-sections drops it even under WHOLE_ARCHIVE. Same
# fix the ESP-IDF docs give for a custom .rodata_custom_desc structure.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,-u,s_fw_image_desc")


# Route every LVGL allocation to PSRAM (lvgl_mem_psram.c). --wrap rather than
# only LVGL's LV_STDLIB_CUSTOM hooks: the allocator is a Kconfig choice and
# sdkconfig.defaults pins it to LV_USE_CLIB_MALLOC, which left those hooks
# compiled out in every build directory. Wrapping works whichever way the
# choice lands, so a regenerated sdkconfig cannot switch the routing back off.
# The -u entries are needed for the same reason as the block below: liblvgl.a
# is scanned after libmain.a, so the __wrap_ references it raises would find
# libmain.a already out of scope.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=lv_malloc_core"
    "-Wl,--wrap=lv_realloc_core"
    "-Wl,--wrap=lv_free_core"
    "-Wl,-u,__wrap_lv_malloc_core"
    "-Wl,-u,__wrap_lv_realloc_core"
    "-Wl,-u,__wrap_lv_free_core"
    # The card's sector layer (sd_sector_io.cpp): PSRAM transfers bounced
    # sixteen sectors a command, single sectors cached (emptied on every card
    # init), every transaction counted for SD_TRACE.
    "-Wl,--wrap=sdmmc_read_sectors"
    "-Wl,--wrap=sdmmc_write_sectors"
    "-Wl,--wrap=sdmmc_card_init"
    # Which computer the board is plugged into, from the control requests it
    # sends while it enumerates (usb_host_profile.cpp). TinyUSB is scanned
    # after libmain.a, hence the -u.
    "-Wl,--wrap=dcd_event_handler"
    "-Wl,-u,__wrap_dcd_event_handler")

# LVGL's allocator hooks (lvgl_mem_psram.c) live in this component, but
# liblvgl__lvgl.a is also scanned *after* libmain.a and the link line carries no
# --start-group, so the objects LVGL pulls in on that second pass would find
# lv_malloc_core & friends already out of reach. Forcing the symbols undefined
# makes the linker take them from this component while it is still in scope.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-u lv_mem_init"
    "-u lv_mem_deinit"
    "-u lv_mem_add_pool"
    "-u lv_mem_remove_pool"
    "-u lv_malloc_core"
    "-u lv_realloc_core"
    "-u lv_free_core"
    "-u lv_mem_monitor_core"
    "-u lv_mem_test_core")


# Ensure tinyusb include directory is added
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")

# CONFIG_TINYUSB_MSC_ENABLED is on for the USB transfer profile, and that also
# pulls in esp_tinyusb's tusb_msc_storage.c — a FATFS-backed storage layer this
# firmware must not have: it answers the host's eject by mounting the card, and
# that mount formats a card FATFS cannot read, while its tud_umount_cb (called
# on every bus reset, in every profile) dereferences a handle that only exists
# in transfer mode. main/usb_transfer.cpp implements the tud_msc_* callbacks
# against the raw card instead. --wrap cannot redirect those: they have weak
# definitions in the very translation units that call them, and ld wraps only
# undefined references. So the file is dropped from the component here; if a
# future esp_tinyusb renames it, the build fails loudly on duplicate symbols.
idf_component_get_property(esp_tusb_lib espressif__esp_tinyusb COMPONENT_LIB)
get_target_property(_esp_tusb_srcs ${esp_tusb_lib} SOURCES)
list(FILTER _esp_tusb_srcs EXCLUDE REGEX "tusb_msc_storage\\.c$")
set_property(TARGET ${esp_tusb_lib} PROPERTY SOURCES ${_esp_tusb_srcs})

# LVGL's Montserrat is replaced by crosspad-gui's (src/fonts/, with European
# diacritics); its component takes LVGL's copies out of the build.
idf_component_get_property(_lvgl_lib lvgl__lvgl COMPONENT_LIB)

# LVGL's assert handler, main/lvgl_assert/lvgl_assert_handler.h, named by
# CONFIG_LV_ASSERT_HANDLER_INCLUDE: lv_assert.h includes it in every target
# that includes lvgl.h, hence PUBLIC. A directory of its own, so no other
# header of main/ becomes visible to them.
target_include_directories(${_lvgl_lib} PUBLIC "${CMAKE_CURRENT_LIST_DIR}/lvgl_assert")

# The firmware is built for size (CONFIG_COMPILER_OPTIMIZATION_SIZE): code and
# rodata run from a copy in PSRAM (SPIRAM_FETCH_INSTRUCTIONS / SPIRAM_RODATA),
# so every kB of image is a kB the UI no longer has. The hot paths keep -O2:
# LVGL's software renderer, and the audio path from I2S through the mixer to
# the USB audio endpoints — at -Os a host loopback over UAC2 glitched ~10
# times a second, at -O2 not once.
target_compile_options(${_lvgl_lib} PRIVATE -O2)
# Not the parts of LVGL no frame waits for: the themes (run once per object
# created, to pick its styles), the observer (subject notifications) and the
# text entry widgets (textarea, keyboard: the Wi-Fi password, a kit's name).
idf_component_get_property(_lvgl_dir lvgl__lvgl COMPONENT_DIR)
file(GLOB _lvgl_cold "${_lvgl_dir}/src/themes/*/*.c" "${_lvgl_dir}/src/others/observer/*.c"
     "${_lvgl_dir}/src/widgets/textarea/*.c" "${_lvgl_dir}/src/widgets/keyboard/*.c")
set_source_files_properties(${_lvgl_cold} DIRECTORY ${_lvgl_dir} PROPERTIES COMPILE_OPTIONS -Os)
foreach(_hot crosspad-mixer crosspad-platform-idf usb_device_uac
             espressif__tinyusb espressif__esp_tinyusb)
    idf_component_get_property(_hot_lib ${_hot} COMPONENT_LIB)
    target_compile_options(${_hot_lib} PRIVATE -O2)
endforeach()
set_source_files_properties(usb_audio_bridge.c
    audio/audio_control.cpp audio/audio_module.cpp audio/audio_task.cpp
    audio/engine_handoff.cpp
    PROPERTIES COMPILE_OPTIONS -O2)
# The pitched engine's voice render and the DSP it loads with, not the rest of
# core: at -Os one voice took 123 us of the 1451 us block (49 at -O2), sixteen
# overran it and the USB capture lost two seconds; all of core at -O2 crosses
# another 64 kB page.
#
# src/sampler/* is the same argument and was simply missed when the native
# sampler engine moved into core. Measured at -Os with a user's pattern at
# 194 BPM: its mixer node cost 807 us of a 1450 us block at eight voices
# (~101 us a voice), `push_us` fell to 78 us -- the render had stopped
# sleeping -- and the worst pass reached 2250 us. The voice does the same
# varispeed the pitched one does, so it pays the same -Os penalty.
idf_component_get_property(_core_dir crosspad-core COMPONENT_DIR)
file(GLOB _core_hot "${_core_dir}/src/instrument/*.cpp" "${_core_dir}/src/dsp/*.cpp"
                    "${_core_dir}/src/sampler/*.cpp")
# The output EQ runs on every block of both codecs; it was -Os with the rest
# of core.
list(APPEND _core_hot "${_core_dir}/src/audio/OutputEq.cpp")
set_source_files_properties(${_core_hot} DIRECTORY ${_core_dir}
    PROPERTIES COMPILE_OPTIONS -O2)
# The native sampler's store of resident heads runs when a kit is taken,
# never in the render: -Os (2026-10-09). So does the take's copy of the kit
# (KitTake.cpp, built on the asking thread).
set_source_files_properties(${_core_dir}/src/sampler/SampleStore.cpp
    ${_core_dir}/src/sampler/KitTake.cpp
    DIRECTORY ${_core_dir} PROPERTIES COMPILE_OPTIONS -Os)
# Files of the -O2 components above that never run per audio block go back to
# -Os: the Mixer's screen, its JSON presets and the desk's JSON (MixerState,
# split out of the engine for this), the BLE MIDI glue, the
# sequences' card writer (a task that wakes every 500 ms), the song engine's
# and the sample tools' board glue (their workers' card and NVS work; the
# render's hooks are one-line forwards to core, which is -Os anyway), the
# settings' NVS store, the engine arena's allocator (kit loads, rings), the
# LED strip (the worker's frame, every 10 ms at most), the Buffer setting's
# port (a choice applied from Settings or a verb, the render parked
# meanwhile), the event bus and the haptics driver (both -Os while they lived
# in core). Not CrosspadPlatform.cpp or PitchedKitPort.cpp: they construct the
# sampler, metronome, USB-input and pitched nodes, so the inline mix() of
# those nodes is compiled in them.
idf_component_get_property(_mixer_dir crosspad-mixer COMPONENT_DIR)
set_source_files_properties(${_mixer_dir}/src/MixerApp.cpp ${_mixer_dir}/src/MixerPresets.cpp
    ${_mixer_dir}/src/MixerState.cpp
    DIRECTORY ${_mixer_dir} PROPERTIES COMPILE_OPTIONS -Os)
idf_component_get_property(_pidf_dir crosspad-platform-idf COMPONENT_DIR)
set_source_files_properties(${_pidf_dir}/EspBleMidi.cpp ${_pidf_dir}/SequenceStorePort.cpp
    ${_pidf_dir}/SongEnginePort.cpp ${_pidf_dir}/SampleToolsPort.cpp ${_pidf_dir}/NvsSettingsStore.cpp
    ${_pidf_dir}/engine_arena.cpp ${_pidf_dir}/EspLedStrip.cpp ${_pidf_dir}/AudioBufferPort.cpp
    ${_pidf_dir}/EspEventBus.cpp ${_pidf_dir}/Drv2605Driver.cpp
    DIRECTORY ${_pidf_dir} PROPERTIES COMPILE_OPTIONS -Os)

# Every verb runs inside hil_control_handle_cdc()'s frame, on the main task's
# stack (CONFIG_ESP_MAIN_TASK_STACK_SIZE, internal RAM): a frame in that file
# past this is a build error here rather than a stack overflow on the board.
# The dispatcher is 1216 B; CDC_STATS stack_min= is what the verbs leave.
set_source_files_properties(hil_control.cpp PROPERTIES COMPILE_OPTIONS -Werror=stack-usage=1280)

if(IDF_TARGET STREQUAL "esp32s3")
    # Endpoint FIFO, deliberately NOT derived from CONFIG_UAC_MIC_INTERVAL_MS.
    # usb_device_uac's header ties the two together; measured against a DAW they
    # want opposite things. At a fixed 5 ms interval, shrinking this buffer made
    # the recorded offset worse, not better: 2156 B -> 12 ms, 1176 B -> 22 ms,
    # 588 B -> 44 ms. A FIFO too small to always have a packet ready makes the
    # host add its own safety buffer, and that costs more than the FIFO saves.
    # So: keep the interval short (task granularity) and the FIFO generous.
    set(CROSSPAD_UAC_EP_IN_BUF 2156)
    message(STATUS "UAC mic interval ${CONFIG_UAC_MIC_INTERVAL_MS} ms, EP IN buffer ${CROSSPAD_UAC_EP_IN_BUF} B (independent)")
    # Add UAC2 audio class support to TinyUSB (enables audio_device.c code paths)
    # Include UAC headers so TinyUSB can resolve descriptor length macros
    target_include_directories(${tusb_lib} PUBLIC
        "${CMAKE_SOURCE_DIR}/components/usb_device_uac/include")
    # Core audio class enable + buffer/format configuration (48kHz, 16-bit, 2ch stereo)
    # USB exposes 2ch stereo; bridge routes same audio to both I2S codecs
    target_compile_definitions(${tusb_lib} PUBLIC
        CFG_TUD_AUDIO=1
        CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP=1
        CFG_TUD_AUDIO_ENABLE_EP_IN=1
        CFG_TUD_AUDIO_ENABLE_EP_OUT=1
        CFG_TUD_AUDIO_FUNC_1_N_FORMATS=1
        CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE=48000
        CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX=2
        CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX=2
        CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX=2
        CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_TX=16
        CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX=2
        CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX=16
        CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_IN=196
        CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_OUT=196
        CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ=${CROSSPAD_UAC_EP_IN_BUF}
        CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX=196
        CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ=3840
        CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX=196
        CFG_TUD_AUDIO_FUNC_1_N_AS_INT=1
        CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ=64
        CFG_TUD_AUDIO_FUNC_1_DESC_LEN=245
    )

    # Full-speed feedback must go out as 10.14, not 16.16. The conversion lives in
    # TinyUSB's audio_device.c, which sees only tusb_config.h and the definitions
    # set on this target -- never components/usb_device_uac/include/tusb_config_uac.h.
    # Setting CONFIG_UAC_SUPPORT_MACOS there alone therefore did nothing: the file
    # kept audio_device.h's default of 0, sent the 4-byte 16.16 value, and macOS
    # answered every packet with a babble error on EP 0x82 (80/s, stream throughput
    # down to 41 %). TinyUSB's own table in audiod_fb_send() lists OSX only on the
    # 10.14 rows; Windows accepts 16.16, which is why this shows up on macOS alone.
    if(CONFIG_UAC_SUPPORT_MACOS)
        target_compile_definitions(${tusb_lib} PUBLIC
            CFG_TUD_AUDIO_ENABLE_FEEDBACK_FORMAT_CORRECTION=1)
        # A TinyUSB without the switch ignores the definition without a word --
        # 0.21 removed it and macOS lost the stream again. Fail here instead.
        idf_component_get_property(tusb_dir espressif__tinyusb COMPONENT_DIR)
        file(STRINGS "${tusb_dir}/src/class/audio/audio_device.c" tusb_fc REGEX "FEEDBACK_FORMAT_CORRECTION")
        if(NOT tusb_fc)
            message(FATAL_ERROR "TinyUSB at ${tusb_dir} has no CFG_TUD_AUDIO_ENABLE_FEEDBACK_FORMAT_CORRECTION: "
                    "full-speed UAC2 feedback would go out as 4-byte 16.16 and macOS drops the stream "
                    "(babble on EP 0x82). Keep espressif/tinyusb at ~0.19 in main/idf_component.yml.")
        endif()
    endif()
endif()


# Submodule revisions for Settings → Info.
include(${_crosspad_root}/components/crosspad-core/cmake/crosspad_git_rev.cmake)
crosspad_git_rev(CROSSPAD_CORE_REV "${CMAKE_SOURCE_DIR}/components/crosspad-core")
crosspad_git_rev(CROSSPAD_GUI_REV  "${CMAKE_SOURCE_DIR}/components/crosspad-gui")
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CROSSPAD_CORE_REV="${CROSSPAD_CORE_REV}"
    CROSSPAD_GUI_REV="${CROSSPAD_GUI_REV}"
)

# Collect assets from crosspad-gui, apps, and project images into staging dir
include(${CMAKE_SOURCE_DIR}/main/collect_assets.cmake)
littlefs_create_partition_image(assets "${ASSET_STAGING_DIR}" FLASH_IN_PROJECT)
