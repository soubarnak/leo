cmake_minimum_required(VERSION 3.21)

project(leo-writer VERSION 0.2.0 LANGUAGES CXX)

# Preview builds are labelled until docs/parity-matrix.md reports every row
# as passed for a stable release (issue #39). Release packages set
# -DLEO_PREVIEW_BUILD=OFF together with a Debian version without the suffix.
option(LEO_PREVIEW_BUILD "Label this build as a preview release" ON)
if(LEO_PREVIEW_BUILD)
  set(LEO_VERSION_STRING "${PROJECT_VERSION}+preview")
else()
  set(LEO_VERSION_STRING "${PROJECT_VERSION}")
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network Widgets)

include(GNUInstallDirs)

add_subdirectory(src)

include(CTest)
if(BUILD_TESTING)
  find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
  add_subdirectory(tests)
endif()

install(FILES resources/io.github.soubarnak.LeoWriter.desktop
  DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES resources/io.github.soubarnak.LeoWriter.svg
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
