add_executable(leo-library-browser-test library_browser_test.cpp)
target_link_libraries(leo-library-browser-test PRIVATE leo_library_browser Qt6::Test Qt6::Widgets)
add_test(NAME leo-library-browser COMMAND leo-library-browser-test)
set_tests_properties(leo-library-browser PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(leo-library-persistence-test library_persistence_test.cpp)
target_link_libraries(leo-library-persistence-test PRIVATE leo_library_browser Qt6::Test Qt6::Widgets)
add_test(NAME leo-library-persistence COMMAND leo-library-persistence-test)
set_tests_properties(leo-library-persistence PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(leo-library-organization-test library_organization_test.cpp)
target_link_libraries(leo-library-organization-test PRIVATE leo_library_browser Qt6::Test Qt6::Widgets)
add_test(NAME leo-library-organization COMMAND leo-library-organization-test)
set_tests_properties(leo-library-organization PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_test(NAME leo-writer-version COMMAND $<TARGET_FILE:leo-writer> --version)
add_test(NAME leo-writer-package-identity
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/package_identity_test.sh
          $<TARGET_FILE:leo-writer>
          ${CMAKE_SOURCE_DIR}/resources/io.github.soubarnak.LeoWriter.desktop
          ${PROJECT_VERSION})

add_executable(leo-writer-runtime-test application_runtime_test.cpp)
target_link_libraries(leo-writer-runtime-test PRIVATE leo_library_browser Qt6::Test Qt6::Core)
add_test(NAME leo-writer-runtime COMMAND leo-writer-runtime-test)
set_tests_properties(leo-writer-runtime PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;LEO_WRITER_TEST_EXECUTABLE=$<TARGET_FILE:leo-writer>")
