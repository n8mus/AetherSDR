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
        src/core/backends/orion/OrionPollScheduler.h)

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
            src/core/backends/orion/SdrPlayApi.h)
    endif()
endif()
