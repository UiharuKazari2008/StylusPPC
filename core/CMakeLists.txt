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
  src/board/irda.cpp  # infrared (SIR) transceiver on UART2 (docs/research-irda.md)
  src/board/audio.cpp
  src/board/folder_share.cpp
  src/board/pcmcia.cpp
  src/board/camera.cpp  # WMDroid camera bridge
  src/board/codepatch.cpp  # host fixes to driver code in guest RAM
  src/board/motion.cpp  # accelerometer for the SEN0: driver
  src/net/usernet.cpp
  src/net/tlsproxy.cpp  # secure web proxy (HTTPS for WM's old TLS), BoringSSL below
  src/net/mwsgate.cpp  # Windows Live Mobile gateway for the built-in WM 6.x client (MSNP to Escargot)
  src/board/snapshot.cpp
  src/htc/romkit.cpp  # apps from the user's HTC ROM (NBH/IMGFS reader, package staging)
  src/board/machine.cpp
  src/util/log.cpp
)
target_include_directories(wmcore PUBLIC src)
if(ANDROID)
  target_link_libraries(wmcore PUBLIC log)
endif()

# Prebuilt BoringSSL (tools/build-boringssl.sh) for the secure web proxy; without it for this ABI
# the proxy is compiled out and HTTPS passes through unchanged.
set(BSSL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/boringssl)
if(ANDROID AND EXISTS ${BSSL_DIR}/lib/${ANDROID_ABI}/libssl.a)
  target_include_directories(wmcore PRIVATE ${BSSL_DIR}/include)
  target_compile_definitions(wmcore PRIVATE WM_HAVE_TLS=1)
  target_link_libraries(wmcore PUBLIC ${BSSL_DIR}/lib/${ANDROID_ABI}/libssl.a ${BSSL_DIR}/lib/${ANDROID_ABI}/libcrypto.a)
endif()

if(WMCORE_CLI)
  add_executable(wmcore-cli cli/main.cpp cli/irda_link.cpp cli/mws_test.cpp cli/thumb_test.cpp)
  target_link_libraries(wmcore-cli wmcore)
  add_executable(htcrom cli/htcrom.cpp)
  target_link_libraries(htcrom wmcore)
endif()
