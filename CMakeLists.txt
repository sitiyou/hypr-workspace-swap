cmake_minimum_required(VERSION 3.25)
project(hypr-workspace-swap VERSION 0.1.0 LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(HYPRLAND REQUIRED IMPORTED_TARGET hyprland=0.57.0)
pkg_check_modules(LUA REQUIRED IMPORTED_TARGET lua)

add_library(hypr-workspace-swap SHARED
    src/main.cpp
    src/Bank.cpp
    src/Hyprland057.cpp
)
target_compile_features(hypr-workspace-swap PRIVATE cxx_std_26)
target_compile_options(hypr-workspace-swap PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
target_link_libraries(hypr-workspace-swap PRIVATE PkgConfig::HYPRLAND PkgConfig::LUA)
set_target_properties(hypr-workspace-swap PROPERTIES PREFIX "")

include(GNUInstallDirs)
install(TARGETS hypr-workspace-swap LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME hypr-workspace-swap-integration
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py
            --plugin $<TARGET_FILE:hypr-workspace-swap>
            --artifacts ${CMAKE_CURRENT_BINARY_DIR}/integration
    )
    set_tests_properties(hypr-workspace-swap-integration PROPERTIES TIMEOUT 180)
endif()
