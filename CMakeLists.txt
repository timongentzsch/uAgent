cmake_minimum_required(VERSION 3.21)
project(uagent VERSION 0.2.0 DESCRIPTION "Lean terminal coding agent" LANGUAGES CXX)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

add_executable(uagent src/main.cpp)
add_library(uagent_options INTERFACE)
target_include_directories(
  uagent_options INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include
                           ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(uagent_options INTERFACE CURL::libcurl Threads::Threads)
target_compile_definitions(uagent_options INTERFACE UAGENT_VERSION="${PROJECT_VERSION}")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(uagent_options INTERFACE -Wall -Wextra -Wpedantic)
endif()
option(UAGENT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
if(UAGENT_WARNINGS_AS_ERRORS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(uagent_options INTERFACE -Werror)
endif()
target_link_libraries(uagent PRIVATE uagent_options)

# Enable LTO where the compiler supports it.
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_ok OUTPUT _)
if(ipo_ok)
  set_property(TARGET uagent PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

# GCC before 9 needs an explicit filesystem library.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
  target_link_libraries(uagent PRIVATE stdc++fs)
endif()

# libedit adds line editing and history when available.
find_library(EDITLINE_LIB NAMES edit)
find_path(EDITLINE_INC editline/readline.h)
if(EDITLINE_LIB AND EDITLINE_INC)
  target_compile_definitions(uagent PRIVATE HAVE_EDITLINE=1)
  target_include_directories(uagent PRIVATE ${EDITLINE_INC})
  target_link_libraries(uagent PRIVATE ${EDITLINE_LIB})
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(uagent_tests tests/test_core.cpp)
  target_link_libraries(uagent_tests PRIVATE uagent_options)
  add_test(NAME core COMMAND uagent_tests)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME integration
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py
                   $<TARGET_FILE:uagent>)
  set_tests_properties(integration PROPERTIES TIMEOUT 120)
  add_test(NAME context_policy_sim
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/context_policy_sim.py)
endif()

option(UAGENT_BUILD_BENCHMARKS "Build lightweight performance baselines" OFF)
if(UAGENT_BUILD_BENCHMARKS)
  add_executable(uagent_bench benchmarks/bench_core.cpp)
  target_link_libraries(uagent_bench PRIVATE uagent_options)
endif()

install(TARGETS uagent RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES README.md SECURITY.md LICENSE docs/ARCHITECTURE.md docs/OPERATIONS.md
              docs/CHECKPOINTS.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

set(CPACK_PACKAGE_NAME "uagent")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Lean C++ terminal coding agent")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "TGZ")
include(CPack)
