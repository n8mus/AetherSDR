# N8EM fork: Ten-Tec Orion backend (family "orion"). See FORK-NOTES.md.
#
# One include from CMakeLists.txt, placed after the Qt6::SerialPort block,
# so every fork build rule lives here and upstream merges touch one line.
#
# The CAT link needs only Qt6::SerialPort. The SDRplay RSP2 panadapter is
# optional (ENABLE_ORION_SDRPLAY below).

option(ENABLE_ORION "Build the Ten-Tec Orion backend (N8EM fork)" ON)

if(NOT ENABLE_ORION)
    message(STATUS "Ten-Tec Orion ... disabled by ENABLE_ORION=OFF")
elseif(NOT Qt6SerialPort_FOUND)
    message(STATUS "Ten-Tec Orion ... disabled — Qt6::SerialPort not found")
else()
    message(STATUS "Ten-Tec Orion ... enabled (serial CAT)")
    set(AETHER_BACKEND_ORION ON)
    target_compile_definitions(aethercore PUBLIC AETHER_BACKEND_ORION)
    target_sources(aethercore PRIVATE
        src/core/backends/orion/OrionBackend.cpp
        src/core/backends/orion/OrionBackend.h
        src/core/backends/orion/OrionCat.cpp
        src/core/backends/orion/OrionCat.h
        src/core/backends/orion/OrionPollScheduler.cpp
        src/core/backends/orion/OrionPollScheduler.h
        src/core/backends/orion/OrionTxAudio.cpp
        src/core/backends/orion/OrionTxAudio.h
        src/core/backends/orion/OrionWinKeyer.cpp
        src/core/backends/orion/OrionWinKeyer.h
        src/core/backends/orion/OrionRotor.cpp
        src/core/backends/orion/OrionRotor.h
        src/core/backends/orion/OrionCwDaemon.cpp
        src/core/backends/orion/OrionCwDaemon.h
        src/core/backends/orion/OrionPatMonitor.cpp
        src/core/backends/orion/OrionPatMonitor.h)
    # CW skimmer (step 8), ported from the Orion console: a bank of CW decoders
    # fed with the RSP2's raw IQ, and a DX-cluster telnet server on :7300.
    target_sources(aethercore PRIVATE
        src/core/backends/orion/skimmer/BayesCwEngine.cpp
        src/core/backends/orion/skimmer/BayesCwEngine.h
        src/core/backends/orion/skimmer/CwDecoder.cpp
        src/core/backends/orion/skimmer/CwDecoder.h
        src/core/backends/orion/skimmer/FldigiCwEngine.cpp
        src/core/backends/orion/skimmer/FldigiCwEngine.h
        src/core/backends/orion/skimmer/SkimServer.cpp
        src/core/backends/orion/skimmer/SkimServer.h
        src/core/backends/orion/skimmer/SkimmerEngine.cpp
        src/core/backends/orion/skimmer/SkimmerEngine.h
        src/core/backends/orion/OrionSkimmer.cpp
        src/core/backends/orion/OrionSkimmer.h
        src/core/backends/orion/OrionSkimmerFeed.cpp
        src/core/backends/orion/OrionSkimmerFeed.h)
    # OpenMP spreads the decoder bank across cores (as the console builds it);
    # optional, and scoped to that one file: without it the bank runs serial.
    find_package(OpenMP QUIET COMPONENTS CXX)
    if(OpenMP_CXX_FOUND)
        set_source_files_properties(src/core/backends/orion/skimmer/SkimmerEngine.cpp
            TARGET_DIRECTORY aethercore
            PROPERTIES COMPILE_OPTIONS "${OpenMP_CXX_FLAGS}")
        target_link_libraries(aethercore PRIVATE OpenMP::OpenMP_CXX)
        message(STATUS "Ten-Tec Orion ... CW skimmer with OpenMP")
    else()
        message(STATUS "Ten-Tec Orion ... CW skimmer without OpenMP (decoder bank runs serial)")
    endif()

    # Station logbook (step 9), ported from the Orion console: its SQLite log,
    # ADIF and the cqrlog bridge. Needs Qt6::Sql (the SQLite driver).
    find_package(Qt6 QUIET COMPONENTS Sql)
    if(TARGET Qt6::Sql)
        set(AETHER_ORION_LOGBOOK ON)
        target_compile_definitions(aethercore PUBLIC AETHER_ORION_LOGBOOK)
        target_link_libraries(aethercore PUBLIC Qt6::Sql)
        target_sources(aethercore PRIVATE
            src/core/backends/orion/log/Adif.cpp
            src/core/backends/orion/log/Adif.h
            src/core/backends/orion/log/CqrlogSync.cpp
            src/core/backends/orion/log/CqrlogSync.h
            src/core/backends/orion/log/LogDb.cpp
            src/core/backends/orion/log/LogDb.h
            src/core/backends/orion/log/LogbookIndex.cpp
            src/core/backends/orion/log/LogbookIndex.h
            src/core/backends/orion/log/WsjtxListener.cpp
            src/core/backends/orion/log/WsjtxListener.h
            src/core/backends/orion/log/OrionLogbook.cpp
            src/core/backends/orion/log/OrionLogbook.h
            src/core/backends/orion/log/OrionUploadSecrets.cpp
            src/core/backends/orion/log/OrionUploadSecrets.h
            src/core/backends/orion/log/QslUploader.cpp
            src/core/backends/orion/log/QslUploader.h)
        target_sources(AetherSDR PRIVATE
            src/gui/orion/OrionLogbookUi.cpp
            src/gui/orion/OrionLogbookUi.h)
        message(STATUS "Ten-Tec Orion ... logbook enabled (Qt6::Sql)")
    else()
        message(STATUS "Ten-Tec Orion ... logbook disabled — Qt6::Sql not found")
    endif()

    # The fork's own GUI page (Radio Setup -> Orion Audio).
    target_sources(AetherSDR PRIVATE
        src/gui/orion/OrionSetupPage.cpp
        src/gui/orion/OrionSetupPage.h
        src/gui/orion/OrionSkimmerUi.cpp
        src/gui/orion/OrionSkimmerUi.h)

    # RSP2 panadapter and receive audio (steps 2-3). Needs the SDRplay API HEADERS from the
    # operator's install and fftw3f. The library itself is dlopen()ed at run
    # time and never linked, so no package can bundle it (fork rule 5).
    option(ENABLE_ORION_SDRPLAY "Orion backend: SDRplay RSP2 panadapter" ON)
    find_path(ORION_SDRPLAY_INCLUDE_DIR sdrplay_api.h)
    if(PkgConfig_FOUND)
        pkg_check_modules(ORION_FFTW3F IMPORTED_TARGET fftw3f)
    endif()
    if(NOT ENABLE_ORION_SDRPLAY)
        message(STATUS "Ten-Tec Orion ... RSP2 panadapter disabled by ENABLE_ORION_SDRPLAY=OFF")
    elseif(NOT ORION_SDRPLAY_INCLUDE_DIR)
        message(STATUS "Ten-Tec Orion ... RSP2 panadapter disabled — sdrplay_api.h not found (install the SDRplay API)")
    elseif(NOT ORION_FFTW3F_FOUND)
        message(STATUS "Ten-Tec Orion ... RSP2 panadapter disabled — fftw3f not found")
    else()
        message(STATUS "Ten-Tec Orion ... RSP2 panadapter enabled (SDRplay API loaded at run time)")
        set(AETHER_ORION_SDRPLAY ON)
        target_compile_definitions(aethercore PRIVATE AETHER_ORION_SDRPLAY)
        target_include_directories(aethercore PRIVATE ${ORION_SDRPLAY_INCLUDE_DIR})
        target_link_libraries(aethercore PRIVATE PkgConfig::ORION_FFTW3F ${CMAKE_DL_LIBS})
        target_sources(aethercore PRIVATE
            src/core/backends/orion/OrionRxAudio.cpp
            src/core/backends/orion/OrionRxAudio.h
            src/core/backends/orion/OrionSpectrum.cpp
            src/core/backends/orion/OrionSpectrum.h
            src/core/backends/orion/Rsp2Receiver.cpp
            src/core/backends/orion/Rsp2Receiver.h
            src/core/backends/orion/SdrPlayApi.cpp
            src/core/backends/orion/SdrPlayApi.h
            src/core/backends/orion/skimmer/SkimStft.cpp
            src/core/backends/orion/skimmer/SkimStft.h)
    endif()
endif()
