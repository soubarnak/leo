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

add_executable(leo-chapter-structure-test chapter_structure_test.cpp)
target_link_libraries(leo-chapter-structure-test PRIVATE leo_library_browser Qt6::Test Qt6::Widgets)
add_test(NAME leo-chapter-structure COMMAND leo-chapter-structure-test)

add_executable(leo-planning-records-test planning_records_test.cpp)
target_link_libraries(leo-planning-records-test PRIVATE leo_library_browser Qt6::Test Qt6::Widgets)
add_test(NAME leo-planning-records COMMAND leo-planning-records-test)
set_tests_properties(leo-chapter-structure PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(leo-darling-records-test darling_records_test.cpp)
target_link_libraries(leo-darling-records-test PRIVATE leo_library_browser Qt6::Test Qt6::Widgets)
add_test(NAME leo-darling-records COMMAND leo-darling-records-test)
set_tests_properties(leo-darling-records PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(leo-book-search-spellcheck-test book_search_spellcheck_test.cpp)
target_link_libraries(leo-book-search-spellcheck-test PRIVATE leo_library_browser Qt6::Test Qt6::Widgets)
add_test(NAME leo-book-search-spellcheck COMMAND leo-book-search-spellcheck-test)
set_tests_properties(leo-book-search-spellcheck PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

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

add_executable(leo-writing-progress-test writing_progress_test.cpp)
target_link_libraries(leo-writing-progress-test PRIVATE leo_library_browser Qt6::Test)
add_test(NAME leo-writing-progress COMMAND leo-writing-progress-test)

add_executable(leo-book-covers-test book_covers_test.cpp)
target_link_libraries(leo-book-covers-test PRIVATE leo_library_browser Qt6::Test Qt6::Widgets)
add_test(NAME leo-book-covers COMMAND leo-book-covers-test)

add_executable(leo-manuscript-import-test manuscript_import_test.cpp)
target_link_libraries(leo-manuscript-import-test PRIVATE leo_library_browser Qt6::Test)
add_test(NAME leo-manuscript-import COMMAND leo-manuscript-import-test)

target_compile_definitions(leo-manuscript-import-test PRIVATE IMPORT_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/import")

set_tests_properties(leo-manuscript-import PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

target_compile_definitions(leo-manuscript-import-test PRIVATE
  IMPORT_NEO_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/neo_import_compatibility.cjs"
  IMPORT_NEO_MAIN="${CMAKE_SOURCE_DIR}/main.js")
