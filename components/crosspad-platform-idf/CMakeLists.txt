# CrossPad Platform — ESP-IDF implementations of crosspad-core interfaces

if(IDF_TARGET STREQUAL "esp32s3")
    set(PLATFORM_S3_SRCS
        "EspStm32MidiTransport.cpp"
        "engine_arena.cpp"
        "UsbAudioPort.cpp"
        "PitchedKitPort.cpp"    # reads the SD card through the BSP mount
        "SamplerEnginePort.cpp" # the native streaming sampler: arena pages, card reads, the node
        "SongEnginePort.cpp"    # audio song: bounce to the card, clips from it
        "SampleToolsPort.cpp"   # recorder and slicer: takes, audition, kits from a loop
        "SamplerPlatformPort.cpp" # the sampler app's callbacks: playback, pad reloads, waveforms
        "InputGainPort.cpp"     # the inputs' preamp: MicAmp PGA, then digital
        "OutputLevelsPort.cpp"  # the speakers' and jacks' analog levels, per codec output pair
        "InputPairPort.cpp"     # which two of mics / Line 1/2 / Line 3/4 the ADCs hear
        "InputMonitorPort.cpp"  # an IN channel sent anywhere holds its ADC on the input
        "AudioBufferPort.cpp"   # the render's block and output queue, chosen live
        "EspDspFft.cpp"         # esp-dsp's FFT, registered as core's FFT backend
        "EspDspKernels.cpp")    # the S3's render kernels (PIE, TRUNC.S/CLAMPS), core's AudioKernelTable
    # ml_sample_stream_player_lib registers nothing unless the prebuilt
    # streamer is built (CONFIG_CROSSPAD_SAMPLER_MARCEL): requirements are
    # resolved before Kconfig is, so the require itself stays unconditional.
    set(PLATFORM_BSP_REQUIRES crosspad stm32_hal ml_sample_stream_player_lib espressif__esp-dsp)
    set(PLATFORM_P4_REQUIRES "")
    set(PLATFORM_HAS_SONG_ENGINE 1)
    # Live's empty Set, 9 kB gzipped, that a song export is built from
    set(PLATFORM_S3_EMBED "../crosspad-core/data/live12_set.xml.gz")
else()
    set(PLATFORM_S3_SRCS "")
    set(PLATFORM_S3_EMBED "")
    set(PLATFORM_BSP_REQUIRES "")
    set(PLATFORM_P4_REQUIRES espressif__esp32_p4_function_ev_board)
    set(PLATFORM_HAS_SONG_ENGINE 0)
endif()

idf_component_register(
    SRCS
        "EspLedStrip.cpp"
        "EspClock.cpp"
        "EspImu.cpp"
        ${PLATFORM_S3_SRCS}
        "IdfAudioModule.cpp"
        "LatTrace.cpp"
        "CrosspadPlatform.cpp"
        "NvsSettingsStore.cpp"
        "SamplePlayerAccess.cpp"
        "SequenceStorePort.cpp"
        "EspBleMidi.cpp"
        "EspEventBus.cpp"
        "Drv2605Driver.cpp"
    INCLUDE_DIRS
        "include"
    EMBED_FILES
        ${PLATFORM_S3_EMBED}
    REQUIRES
        crosspad-core
    PRIV_REQUIRES
        esp_event
        esp_timer
        driver
        fatfs
        nvs_flash
        espressif__led_strip
        ${PLATFORM_BSP_REQUIRES}
        ${PLATFORM_P4_REQUIRES}
        crosspad-mixer
        crosspad-sampler   # the engine reports its voices to the Sampler's UI
        arduino_midi_library
        bt
)

# The song engine is built only where the SD card and PSRAM rings are (the
# S3 list above); the render calls into it only there.
target_compile_definitions(${COMPONENT_LIB} PRIVATE CROSSPAD_HAS_SONG_ENGINE=${PLATFORM_HAS_SONG_ENGINE})
