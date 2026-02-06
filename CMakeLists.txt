cmake_minimum_required(VERSION 3.15)
project(ScreenCapture LANGUAGES CXX)

# Pick source file (handles either ScreenCappture.cpp or ScreenCapture.cpp)
set(SRC_CANDIDATE_1 "${CMAKE_CURRENT_SOURCE_DIR}/ScreenCappture.cpp")
set(SRC_CANDIDATE_2 "${CMAKE_CURRENT_SOURCE_DIR}/ScreenCapture.cpp")
if(EXISTS "${SRC_CANDIDATE_1}")
  set(SRC "${SRC_CANDIDATE_1}")
elseif(EXISTS "${SRC_CANDIDATE_2}")
  set(SRC "${SRC_CANDIDATE_2}")
else()
  message(FATAL_ERROR "Neither ScreenCappture.cpp nor ScreenCapture.cpp found")
endif()

add_library(ScreenCapture SHARED "${SRC}")

target_compile_features(ScreenCapture PRIVATE cxx_std_17)
target_compile_definitions(ScreenCapture PRIVATE UNICODE _UNICODE)

# Link required Windows libraries
if(MSVC)
  target_link_libraries(ScreenCapture PRIVATE Gdiplus Shlwapi Gdi32 User32 Dwmapi)
else()
  target_link_libraries(ScreenCapture PRIVATE gdiplus shlwapi gdi32 user32 dwmapi)
endif()

set_target_properties(ScreenCapture PROPERTIES OUTPUT_NAME "ScreenCapture")

# Keep undecorated export name on 32-bit by using a .def file
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
  target_sources(ScreenCapture PRIVATE ScreenCapture.def)
  set_property(TARGET ScreenCapture PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ScreenCapture.def")
endif()
