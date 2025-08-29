cmake_minimum_required(VERSION 3.13)
project(llama_cli LANGUAGES C CXX)

# Options
option(WITH_VOICE_IO "Enable voice STT/TTS (Vosk + PortAudio + eSpeak)" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
find_package(Threads REQUIRED)

# llama.cpp exports targets "llama" and "common" at top-level.
# Ensure this CMakeLists is added from within the same build (recommended).
# If building standalone, add a find_package(llama CONFIG REQUIRED) here instead.
#
# add_executable target
set(TARGET llama-cli)
add_executable(${TARGET} main.cpp)

# Always link the core llama.cpp bits
target_link_libraries(${TARGET} PRIVATE
    llama
    common
    Threads::Threads
)

# -------------------------
# Optional Voice I/O block:
# -------------------------
if (WITH_VOICE_IO)
  message(STATUS "Voice I/O enabled: looking for PortAudio, Vosk, and eSpeak")

  # Prefer pkg-config for PortAudio
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)
  target_include_directories(${TARGET} PRIVATE ${PORTAUDIO_INCLUDE_DIRS})
  target_link_libraries(${TARGET} PRIVATE ${PORTAUDIO_LIBRARIES})

  # Vosk (no pkg-config expected) – find headers + lib directly
  # Your stt.bb installs:   ${includedir}/vosk_api.h   and   ${libdir}/libvosk.so
  find_path(VOSK_INCLUDE_DIR
    NAMES vosk_api.h
    PATH_SUFFIXES .
    PATHS /usr/include
  )
  find_library(VOSK_LIBRARY NAMES vosk)

  if (NOT VOSK_INCLUDE_DIR OR NOT VOSK_LIBRARY)
    message(FATAL_ERROR "Vosk not found: set VOSK_INCLUDE_DIR and VOSK_LIBRARY or install stt (vosk-api) properly")
  endif()

  target_include_directories(${TARGET} PRIVATE ${VOSK_INCLUDE_DIR})
  target_link_libraries(${TARGET} PRIVATE ${VOSK_LIBRARY})

  # eSpeak (classic) first; if you later switch to espeak-ng, this will prefer espeak-ng
  pkg_check_modules(ESPEAKNG QUIET espeak-ng)
  if (ESPEAKNG_FOUND)
    message(STATUS "Using eSpeak-NG")
    target_compile_definitions(${TARGET} PRIVATE HAVE_ESPEAK_NG)
    target_include_directories(${TARGET} PRIVATE ${ESPEAKNG_INCLUDE_DIRS})
    target_link_libraries(${TARGET} PRIVATE ${ESPEAKNG_LIBRARIES})
  else()
    pkg_check_modules(ESPEAK REQUIRED espeak)
    message(STATUS "Using classic eSpeak")
    target_compile_definitions(${TARGET} PRIVATE HAVE_ESPEAK)
    target_include_directories(${TARGET} PRIVATE ${ESPEAK_INCLUDE_DIRS})
    target_link_libraries(${TARGET} PRIVATE ${ESPEAK_LIBRARIES})
  endif()
endif()

# Install binary
install(TARGETS ${TARGET}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
