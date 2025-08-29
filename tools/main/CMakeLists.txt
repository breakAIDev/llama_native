cmake_minimum_required(VERSION 3.13)
project(llama_cli LANGUAGES C CXX)

option(WITH_VOICE_IO "Enable voice STT/TTS (Vosk + PortAudio + eSpeak)" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

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
  target_compile_definitions(${TARGET} PRIVATE HAVE_VOICE_IO)

  # ------------------ PortAudio ------------------
  pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)
  target_include_directories(${TARGET} PRIVATE ${PORTAUDIO_INCLUDE_DIRS})
  target_link_libraries(${TARGET} PRIVATE ${PORTAUDIO_LIBRARIES})

  # ------------------ Vosk (STT) -----------------
  # Accept Yocto hints (-DVOSK_INCLUDE_DIR, -DVOSK_LIBRARY); otherwise try to find.
  if (DEFINED VOSK_INCLUDE_DIR)
    message(STATUS "Using VOSK_INCLUDE_DIR hint: ${VOSK_INCLUDE_DIR}")
  else()
    find_path(VOSK_INCLUDE_DIR
      NAMES vosk_api.h
      HINTS
        $ENV{VOSK_INCLUDE_DIR}
        ${CMAKE_INSTALL_PREFIX}/include
      PATHS
        /usr/include /usr/local/include
    )
  endif()

  if (DEFINED VOSK_LIBRARY)
    message(STATUS "Using VOSK_LIBRARY hint: ${VOSK_LIBRARY}")
  else()
    find_library(VOSK_LIBRARY
      NAMES vosk
      HINTS
        $ENV{VOSK_LIBRARY}
        ${CMAKE_INSTALL_PREFIX}/lib ${CMAKE_INSTALL_PREFIX}/lib64
      PATHS
        /usr/lib /usr/lib64 /usr/local/lib /usr/local/lib64
    )
  endif()

  if (NOT VOSK_INCLUDE_DIR OR NOT VOSK_LIBRARY)
    message(FATAL_ERROR "Vosk not found: set VOSK_INCLUDE_DIR and VOSK_LIBRARY or ensure the vosk (stt) package is installed")
  endif()

  target_include_directories(${TARGET} PRIVATE ${VOSK_INCLUDE_DIR})
  target_link_libraries(${TARGET} PRIVATE ${VOSK_LIBRARY})
  target_compile_definitions(${TARGET} PRIVATE HAVE_VOSK)

  # ------------------ eSpeak TTS -----------------
  # Prefer eSpeak-NG via pkg-config
  pkg_check_modules(ESPEAKNG QUIET espeak-ng)
  if (ESPEAKNG_FOUND)
    message(STATUS "Using eSpeak-NG via pkg-config")
    target_compile_definitions(${TARGET} PRIVATE HAVE_ESPEAK_NG)
    target_include_directories(${TARGET} PRIVATE ${ESPEAKNG_INCLUDE_DIRS})
    target_link_libraries(${TARGET} PRIVATE ${ESPEAKNG_LIBRARIES})
  else()
    # Try classic eSpeak via pkg-config
    pkg_check_modules(ESPEAK_PKG QUIET espeak)
    if (ESPEAK_PKG_FOUND)
      message(STATUS "Using classic eSpeak via pkg-config")
      target_compile_definitions(${TARGET} PRIVATE HAVE_ESPEAK)
      target_include_directories(${TARGET} PRIVATE ${ESPEAK_PKG_INCLUDE_DIRS})
      target_link_libraries(${TARGET} PRIVATE ${ESPEAK_PKG_LIBRARIES})
    else()
      # Final fallback: direct header/lib
      find_path(ESPEAK_INCLUDE_DIR NAMES espeak/speak_lib.h
        PATHS /usr/include /usr/local/include
      )
      find_library(ESPEAK_LIBRARY NAMES espeak
        PATHS /usr/lib /usr/lib64 /usr/local/lib /usr/local/lib64
      )

      if (ESPEAK_INCLUDE_DIR AND ESPEAK_LIBRARY)
        message(STATUS "Using classic eSpeak via direct header/library")
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
