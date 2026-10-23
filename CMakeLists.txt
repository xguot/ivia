cmake_minimum_required(VERSION 3.22)
project(ivia VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.6 REQUIRED COMPONENTS Widgets Pdf)

add_library(ivia_core STATIC
    src/command.cpp src/command.h
    src/command_line.cpp src/command_line.h
    src/reader_view.cpp src/reader_view.h
    src/main_window.cpp src/main_window.h
    src/demo.cpp src/demo.h src/theme.h
)
target_include_directories(ivia_core PUBLIC src)
target_link_libraries(ivia_core PUBLIC Qt6::Widgets Qt6::Pdf)
target_compile_options(ivia_core PRIVATE
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall;-Wextra;-Wpedantic>)

qt_add_executable(ivia MACOSX_BUNDLE src/main.cpp)
target_link_libraries(ivia PRIVATE ivia_core)
set_target_properties(ivia PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER org.ivia.reader
    MACOSX_BUNDLE_BUNDLE_NAME Ivia
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist.in")

include(GNUInstallDirs)
install(TARGETS ivia BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
qt_generate_deploy_app_script(TARGET ivia OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${deploy_script})

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.6 REQUIRED COMPONENTS Test)
    qt_add_executable(ivia_tests tests/reader_test.cpp)
    target_link_libraries(ivia_tests PRIVATE ivia_core Qt6::Test)
    add_test(NAME ivia_tests COMMAND ivia_tests)
    set_tests_properties(ivia_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME ivia_hidpi_render COMMAND ivia_tests visibleRastersSettle)
    set_tests_properties(ivia_hidpi_render PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=2")
    qt_add_executable(ivia_command_path_tests tests/command_path_test.cpp)
    target_link_libraries(ivia_command_path_tests PRIVATE ivia_core Qt6::Test)
    add_test(NAME ivia_command_path_tests COMMAND ivia_command_path_tests)
endif()
