cmake_minimum_required(VERSION 3.22)
project(wmcore CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g")

add_library(wmcore STATIC
  src/arm/cpu.cpp
  src/arm/disasm.cpp
  src/arm/jit.cpp
  src/board/bus.cpp
  src/board/s3c2410.cpp
  src/board/devices.cpp
  src/board/de_devices.cpp
  src/board/nled.cpp  # notification LED / vibrator decoder
  src/board/modem.cpp  # virtual GSM modem on UART0 (rilgsm images)
  src/board/audio.cpp
  src/board/folder_share.cpp
  src/board/pcmcia.cpp
  src/board/camera.cpp  # WMDroid camera bridge
  src/board/codepatch.cpp  # host fixes to driver code in guest RAM
  src/net/usernet.cpp
  src/board/snapshot.cpp
  src/board/machine.cpp
  src/util/log.cpp
)
target_include_directories(wmcore PUBLIC src)
if(ANDROID)
  target_link_libraries(wmcore PUBLIC log)
endif()

if(WMCORE_CLI)
  add_executable(wmcore-cli cli/main.cpp)
  target_link_libraries(wmcore-cli wmcore)
endif()
