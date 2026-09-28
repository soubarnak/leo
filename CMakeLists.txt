cmake_minimum_required(VERSION 3.21)

project(leo-writer VERSION 0.2.0 LANGUAGES CXX)

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
