cmake_minimum_required(VERSION 3.25)

project(ALK VERSION 0.1.0 LANGUAGES CXX)

option(ALK_BUILD_TESTS "Build the dependency-free ALK test suite" ON)

add_library(libalk
    src/libalk.cpp
)
add_library(ALK::libalk ALIAS libalk)

target_compile_features(libalk PUBLIC cxx_std_23)
target_include_directories(libalk
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
set_target_properties(libalk PROPERTIES
    CXX_EXTENSIONS OFF
    OUTPUT_NAME alk
)

if(MSVC)
    target_compile_options(libalk PRIVATE /W4 /permissive-)
else()
    target_compile_options(libalk PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

add_executable(alk cli/main.cpp)
target_compile_features(alk PRIVATE cxx_std_23)
target_link_libraries(alk PRIVATE ALK::libalk)

if(ALK_BUILD_TESTS)
    enable_testing()
    add_executable(alk_tests tests/runtime_tests.cpp)
    target_compile_features(alk_tests PRIVATE cxx_std_23)
    target_link_libraries(alk_tests PRIVATE ALK::libalk)
    add_test(NAME alk.runtime COMMAND alk_tests)
endif()
