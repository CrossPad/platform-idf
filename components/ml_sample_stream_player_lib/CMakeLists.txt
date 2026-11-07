# The prebuilt streamer is built only when it is asked for
# (CONFIG_CROSSPAD_SAMPLER_MARCEL, components/crosspad-platform-idf/Kconfig):
# the native engine plays every kit without it, and its archive and glue are
# ~25 kB of image -- which is PSRAM too (CLAUDE.md, "The image size is PSRAM
# too"). Off, this registers an empty component so a stale REQUIRES still
# resolves, and nothing links the archive.
if(CONFIG_CROSSPAD_SAMPLER_MARCEL)
    file(
        GLOB_RECURSE
            SRC_FILES
                "src/*.cpp"
    )

    idf_component_register(
        SRCS
            ${SRC_FILES}
        INCLUDE_DIRS
            "src"
    )

    target_link_libraries(
        ${COMPONENT_LIB} INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/esp32s3/libcrosspad_ply.a
    )
else()
    idf_component_register()
endif()
