cmake_minimum_required(VERSION 3.22)
project(wmdroid CXX)
set(CMAKE_CXX_STANDARD 17)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../core ${CMAKE_BINARY_DIR}/wmcore)
add_library(wmdroid SHARED jni.cpp audio_host.cpp gl_renderer.cpp camera_jni.cpp irda_jni.cpp motion_jni.cpp htcrom_jni.cpp)  # camera_jni.cpp: camera bridge; irda_jni.cpp: infrared; motion_jni.cpp: G-sensor; htcrom_jni.cpp: apps from an HTC ROM
target_link_libraries(wmdroid wmcore android log aaudio EGL GLESv2)
