cmake_minimum_required(VERSION 3.13)
project(llama_cli LANGUAGES C CXX)

option(WITH_VOICE_IO "Enable voice STT/TTS (Vosk + PortAudio + eSpeak)" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
find_package(Threads REQUIRED)

# llama.cpp exports targets "llama" and "common" in the same build.
# If building separately, add: find_package(llama CONFIG REQUIRED)

set(TARGET llama-cli)
add_executable(${TARGET} main.cpp)

target_link_libraries(${TARGET} PRIVATE
    llama
    common
    Threads::Threads
)

if (WITH_VOICE_IO)
  message(STATUS "Voice I/O enabled: looking for PortAudio, Vosk, and eSpeak")

  # -------- PortAudio (pkg-config) --------
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)
  target_include_directories(${TARGET} PRIVATE ${PORTAUDIO_INCLUDE_DIRS})
  target_link_libraries(${TARGET} PRIVATE ${PORTAUDIO_LIBRARIES})

  # -------- Vosk (no pkg-config expected) --------
  # Your stt.bb installs: ${includedir}/vosk_api.h and ${libdir}/libvosk.so
  find_path(VOSK_INCLUDE_DIR
    NAMES vosk_api.h
  )
  find_library(VOSK_LIBRARY NAMES vosk)

  if (NOT VOSK_INCLUDE_DIR OR NOT VOSK_LIBRARY)
    message(FATAL_ERROR "Vosk not found: set VOSK_INCLUDE_DIR and VOSK_LIBRARY or ensure stt (vosk-api) is installed")
  endif()

  target_include_directories(${TARGET} PRIVATE ${VOSK_INCLUDE_DIR})
  target_link_libraries(${TARGET} PRIVATE ${VOSK_LIBRARY})

  # -------- TTS (prefer espeak-ng, fallback to classic espeak) --------
  # 1) Try espeak-ng via pkg-config
  pkg_check_modules(ESPEAKNG QUIET espeak-ng)
  if (ESPEAKNG_FOUND)
    message(STATUS "Using eSpeak-NG via pkg-config")
    target_compile_definitions(${TARGET} PRIVATE HAVE_ESPEAK_NG)
    target_include_directories(${TARGET} PRIVATE ${ESPEAKNG_INCLUDE_DIRS})
    target_link_libraries(${TARGET} PRIVATE ${ESPEAKNG_LIBRARIES})
  else()
    # 2) Try classic espeak via pkg-config (often missing .pc in Yocto)
    pkg_check_modules(ESPEAK_PKG QUIET espeak)
    if (ESPEAK_PKG_FOUND)
      message(STATUS "Using classic eSpeak via pkg-config")
      target_compile_definitions(${TARGET} PRIVATE HAVE_ESPEAK)
      target_include_directories(${TARGET} PRIVATE ${ESPEAK_PKG_INCLUDE_DIRS})
      target_link_libraries(${TARGET} PRIVATE ${ESPEAK_PKG_LIBRARIES})
    else()
      # 3) Fallback: find headers & libs directly (works in Yocto sysroot)
      find_path(ESPEAK_INCLUDE_DIR NAMES espeak/speak_lib.h)
      find_library(ESPEAK_LIBRARY NAMES espeak)

      if (ESPEAK_INCLUDE_DIR AND ESPEAK_LIBRARY)
        message(STATUS "Using classic eSpeak via direct find_path/find_library")
        target_compile_definitions(${TARGET} PRIVATE HAVE_ESPEAK)
        target_include_directories(${TARGET} PRIVATE ${ESPEAK_INCLUDE_DIR})
        target_link_libraries(${TARGET} PRIVATE ${ESPEAK_LIBRARY})
      else()
        message(FATAL_ERROR "eSpeak/eSpeak-NG not found (no pkg-config and no headers/libs)")
      endif()
    endif()
  endif()
endif()

install(TARGETS ${TARGET} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
