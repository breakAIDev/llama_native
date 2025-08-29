cmake_minimum_required(VERSION 3.13)
project(llama_cli CXX)

include(GNUInstallDirs)

# Turn this OFF to build the classic keyboard-only CLI
option(WITH_VOICE_IO "Enable mic STT (Vosk+PortAudio) and TTS (eSpeak NG)" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

# Your existing target
set(TARGET llama-cli)
add_executable(${TARGET} main.cpp)
target_compile_features(${TARGET} PRIVATE cxx_std_17)

# Link to llama.cpp's libs as you already do
target_link_libraries(${TARGET} PRIVATE common llama Threads::Threads)

# ---- Optional Voice I/O (STT+TTS) -------------------------------------------
if (WITH_VOICE_IO)
  find_package(PkgConfig REQUIRED)

  # PortAudio & eSpeak-NG are exposed via pkg-config on Yocto
  pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)
  pkg_check_modules(ESPEAKNG REQUIRED espeak-ng)

  # Vosk doesn't always ship a .pc file; find it the old-fashioned way
  # (Yocto sysroot paths are honored via CMAKE_FIND_ROOT_PATH)
  find_path(VOSK_INCLUDE_DIR
    NAMES vosk_api.h
    HINTS ENV VOSK_INCLUDE_DIR
    PATHS
      ${CMAKE_FIND_ROOT_PATH}/usr/include
      /usr/include
      /usr/local/include
  )
  find_library(VOSK_LIB
    NAMES vosk
    HINTS ENV VOSK_LIB
    PATHS
      ${CMAKE_FIND_ROOT_PATH}/usr/lib ${CMAKE_FIND_ROOT_PATH}/usr/lib64
      /usr/lib /usr/lib64 /usr/local/lib /usr/local/lib64
  )

  if (NOT VOSK_INCLUDE_DIR OR NOT VOSK_LIB)
    message(FATAL_ERROR "WITH_VOICE_IO=ON but Vosk not found. Provide -DVOSK_INCLUDE_DIR=... and -DVOSK_LIB=...")
  endif()

  target_compile_definitions(${TARGET} PRIVATE WITH_VOICE_IO)

  target_include_directories(${TARGET} PRIVATE
    ${PORTAUDIO_INCLUDE_DIRS}
    ${ESPEAKNG_INCLUDE_DIRS}
    ${VOSK_INCLUDE_DIR}
  )

  target_link_libraries(${TARGET} PRIVATE
    ${PORTAUDIO_LIBRARIES}
    ${ESPEAKNG_LIBRARIES}
    ${VOSK_LIB}
  )

  # Honor any extra flags pkg-config wants (usually empty on Yocto, but safe)
  target_compile_options(${TARGET} PRIVATE
    ${PORTAUDIO_CFLAGS_OTHER}
    ${ESPEAKNG_CFLAGS_OTHER}
  )
  target_link_options(${TARGET} PRIVATE
    ${PORTAUDIO_LDFLAGS_OTHER}
    ${ESPEAKNG_LDFLAGS_OTHER}
  )
endif()

# Install into /usr/bin (Yocto respects ${CMAKE_INSTALL_BINDIR})
install(TARGETS ${TARGET} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
