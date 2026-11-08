cmake_minimum_required(VERSION 3.28...3.30)

find_package(Qt6 REQUIRED Widgets)

add_library(snip-record MODULE)
add_library(OBS::snip-record ALIAS snip-record)

target_sources(
  snip-record
  PRIVATE snip-record.cpp snip-picker.cpp snip-picker.hpp snip-recorder.cpp snip-recorder.hpp snip-geometry.hpp
          snip-selection.hpp
)

target_link_libraries(snip-record PRIVATE OBS::frontend-api OBS::libobs Qt::Widgets)

if(OS_WINDOWS)
  target_link_libraries(snip-record PRIVATE dwmapi)
  configure_file(cmake/windows/obs-module.rc.in snip-record.rc)
  target_sources(snip-record PRIVATE snip-record.rc)
  set_property(TARGET snip-record APPEND PROPERTY AUTORCC_OPTIONS --format-version 1)
endif()

set_target_properties_obs(snip-record PROPERTIES FOLDER plugins PREFIX "" AUTOMOC ON)
