add_library(leo_platform STATIC
  app_paths.cpp
  error_log.cpp
  single_instance.cpp)
target_include_directories(leo_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(leo_platform PUBLIC Qt6::Core Qt6::Network)
target_compile_options(leo_platform PRIVATE -Wall -Wextra -Wpedantic)

add_library(leo_library_browser STATIC
  legacy_chapter_codec.cpp
  library_persistence.cpp
  library_reader.cpp
  library_window.cpp
  release_check_dialog.cpp)
target_include_directories(leo_library_browser PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(leo_library_browser PUBLIC leo_platform Qt6::Core Qt6::Network Qt6::Widgets)
target_compile_options(leo_library_browser PRIVATE -Wall -Wextra -Wpedantic)

add_executable(leo-writer main.cpp)
target_link_libraries(leo-writer PRIVATE leo_library_browser)
target_compile_definitions(leo-writer PRIVATE LEO_VERSION="${PROJECT_VERSION}")
target_compile_options(leo-writer PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS leo-writer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
