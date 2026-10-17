# N8EM fork: Ten-Tec Orion backend (family "orion"). See FORK-NOTES.md.
#
# One include from CMakeLists.txt, placed after the Qt6::SerialPort block,
# so every fork build rule lives here and upstream merges touch one line.
#
# Step 1 is serial CAT only and needs nothing beyond Qt6::SerialPort. The
# SDRplay RSP2 (step 2) will add an ENABLE_SDRPLAY switch here; the
# proprietary library is linked from the system, never bundled (fork rule 5).

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
endif()
