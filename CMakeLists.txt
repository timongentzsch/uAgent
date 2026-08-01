cmake_minimum_required(VERSION 3.21)
project(uagent VERSION 0.2.0 DESCRIPTION "Lean terminal coding agent" LANGUAGES CXX)
include(GNUInstallDirs)

if(NOT UNIX)
  message(FATAL_ERROR "uagent currently requires a POSIX platform")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

add_library(uagent_options INTERFACE)
target_include_directories(uagent_options INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uagent_options INTERFACE CURL::libcurl Threads::Threads)
target_compile_definitions(uagent_options INTERFACE UAGENT_VERSION="${PROJECT_VERSION}")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(uagent_options INTERFACE -fno-exceptions -Wall -Wextra -Wpedantic)
endif()
option(UAGENT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
if(UAGENT_WARNINGS_AS_ERRORS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(uagent_options INTERFACE -Werror)
endif()
option(UAGENT_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(UAGENT_ENABLE_COVERAGE "Enable coverage instrumentation" OFF)
option(UAGENT_BUILD_FUZZERS "Build libFuzzer targets" OFF)
if(UAGENT_ENABLE_TSAN)
  target_compile_options(uagent_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(uagent_options INTERFACE -fsanitize=thread)
endif()
if(UAGENT_ENABLE_COVERAGE)
  target_compile_options(uagent_options INTERFACE --coverage -O0 -g)
  target_link_options(uagent_options INTERFACE --coverage)
endif()
if(UAGENT_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "UAGENT_BUILD_FUZZERS requires Clang")
  endif()
  target_compile_options(uagent_options INTERFACE -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
  target_link_options(uagent_options INTERFACE -fsanitize=address,undefined)
endif()

add_library(
  uagent_core
  src/agent/agent.cc
  src/agent/checkpoint.cc
  src/agent/context_policy.cc
  src/agent/conversation.cc
  src/agent/request.cc
  src/agent/session_store.cc
  src/agent/tool_loop.cc
  src/agent/turn.cc
  src/api/client.cc
  src/api/openai_stream.cc
  src/app/application.cc
  src/app/bootstrap.cc
  src/app/eval.cc
  src/app/options.cc
  src/app/runtime.cc
  src/cli.cc
  src/core/child_env.cc
  src/core/debug.cc
  src/core/env.cc
  src/core/platform_posix.cc
  src/core/signals.cc
  src/core/steering.cc
  src/core/strings.cc
  src/media/attachments.cc
  src/providers.cc
  src/transport/sse.cc
  src/ui/conversation.cc)
target_link_libraries(uagent_core PUBLIC uagent_options)

add_executable(uagent src/main.cc)
target_link_libraries(uagent PRIVATE uagent_core)

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_ok OUTPUT _)
if(ipo_ok)
  set_property(TARGET uagent PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

find_library(EDITLINE_LIB NAMES edit)
find_path(EDITLINE_INC editline/readline.h)
if(EDITLINE_LIB AND EDITLINE_INC)
  target_compile_definitions(uagent_options INTERFACE HAVE_EDITLINE=1)
  target_include_directories(uagent_options INTERFACE ${EDITLINE_INC})
  target_link_libraries(uagent_options INTERFACE ${EDITLINE_LIB})
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(
    uagent_tests
    tests/unit/config_test.cc
    tests/unit/conversation_test.cc
    tests/unit/filesystem_test.cc
    tests/unit/mcp_session_test.cc
    tests/unit/protocol_test.cc
    tests/unit/runtime_test.cc
    tests/unit/skills_test.cc
    tests/unit/stream_test.cc
    tests/unit/test_main.cc
    tests/unit/tools_test.cc)
  target_link_libraries(uagent_tests PRIVATE uagent_core)
  add_test(NAME core COMMAND uagent_tests)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME integration
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py
                   $<TARGET_FILE:uagent>)
  set_tests_properties(integration PROPERTIES TIMEOUT 240)
  add_test(NAME context_policy_sim
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/context_policy_sim.py)
  add_test(NAME live_evaluation_helpers
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/live_evaluation_test.py)
endif()

option(UAGENT_BUILD_BENCHMARKS "Build lightweight performance baselines" OFF)
if(UAGENT_BUILD_BENCHMARKS)
  add_executable(uagent_bench benchmarks/bench_core.cc)
  target_link_libraries(uagent_bench PRIVATE uagent_core)
endif()

if(UAGENT_BUILD_FUZZERS)
  foreach(target IN ITEMS sse openai_stream)
    add_executable(uagent_fuzz_${target} tests/fuzz/fuzz_${target}.cc)
    target_link_libraries(uagent_fuzz_${target} PRIVATE uagent_core)
    target_link_options(uagent_fuzz_${target} PRIVATE -fsanitize=fuzzer)
  endforeach()
endif()

install(TARGETS uagent RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS tests/agent_workflow_live.py
        DESTINATION ${CMAKE_INSTALL_DATADIR}/uagent RENAME eval.py)
install(FILES README.md SECURITY.md CONTRIBUTING.md LICENSE
              docs/ARCHITECTURE.md docs/OPERATIONS.md docs/CHECKPOINTS.md
              docs/PERSISTENCE.md docs/TESTING.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

set(CPACK_PACKAGE_NAME "uagent")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Lean C++ terminal coding agent")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "TGZ")
include(CPack)
