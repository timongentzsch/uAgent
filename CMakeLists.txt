cmake_minimum_required(VERSION 3.21)
project(uagent VERSION 0.8.0 DESCRIPTION "Lean terminal coding agent" LANGUAGES CXX)
include(GNUInstallDirs)

if(NOT UNIX)
  message(FATAL_ERROR "uagent currently requires a POSIX platform")
endif()

set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(_IS_GCC_CLANG OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  set(_IS_GCC_CLANG ON)
endif()

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

add_library(uagent_options INTERFACE)
target_include_directories(uagent_options INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uagent_options INTERFACE CURL::libcurl Threads::Threads)
target_compile_features(uagent_options INTERFACE cxx_std_20)
target_compile_definitions(uagent_options INTERFACE UAGENT_VERSION="${PROJECT_VERSION}")
if(_IS_GCC_CLANG)
  # Narrowing and signedness are load-bearing here: every limit in the agent is
  # int64_t arithmetic that ends up indexing or sizing something unsigned.
  target_compile_options(
    uagent_options INTERFACE -fno-exceptions -Wall -Wextra -Wpedantic
                             -Wconversion -Wsign-conversion -Wshadow
                             -Wold-style-cast)
endif()
if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 20)
  # Xcode 16 ships C++20 stop_token/jthread behind this libc++ feature gate.
  target_compile_options(uagent_options INTERFACE -fexperimental-library)
endif()
option(UAGENT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
if(UAGENT_WARNINGS_AS_ERRORS AND _IS_GCC_CLANG)
  target_compile_options(uagent_options INTERFACE -Werror)
endif()
option(UAGENT_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(UAGENT_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(UAGENT_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(UAGENT_ENABLE_COVERAGE "Enable coverage instrumentation" OFF)
option(UAGENT_BUILD_FUZZERS "Build libFuzzer targets" OFF)
if(UAGENT_ENABLE_TSAN AND (UAGENT_ENABLE_ASAN OR UAGENT_ENABLE_UBSAN))
  message(FATAL_ERROR "ThreadSanitizer cannot be combined with ASan or UBSan")
endif()
if(UAGENT_ENABLE_TSAN AND UAGENT_BUILD_FUZZERS)
  message(FATAL_ERROR "ThreadSanitizer cannot be combined with fuzzing")
endif()
if((UAGENT_ENABLE_ASAN OR UAGENT_ENABLE_UBSAN OR UAGENT_ENABLE_TSAN) AND NOT _IS_GCC_CLANG)
  message(FATAL_ERROR "Sanitizers require Clang or GCC")
endif()
set(_sans)
if(UAGENT_ENABLE_ASAN)
  list(APPEND _sans address)
endif()
if(UAGENT_ENABLE_UBSAN)
  list(APPEND _sans undefined)
endif()
if(UAGENT_ENABLE_TSAN)
  list(APPEND _sans thread)
endif()
if(_sans)
  string(JOIN "," _sans_flags ${_sans})
  target_compile_options(uagent_options INTERFACE -fsanitize=${_sans_flags} -fno-omit-frame-pointer)
  target_link_options(uagent_options INTERFACE -fsanitize=${_sans_flags})
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
  src/agent/conversation.cc
  src/agent/prompt.cc
  src/agent/request.cc
  src/agent/session_store.cc
  src/agent/tool_loop.cc
  src/agent/trace.cc
  src/agent/turn.cc
  src/api/capabilities.cc
  src/api/client.cc
  src/api/openai_stream.cc
  src/api/wire_request.cc
  src/api/wire_stream.cc
  src/app/application.cc
  src/app/bootstrap.cc
  src/app/commands.cc
  src/app/config_proposal.cc
  src/app/options.cc
  src/app/reference.cc
  src/app/runtime.cc
  src/app/self_description.cc
  src/cli.cc
  src/core/child_env.cc
  src/core/config_document.cc
  src/core/config_registry.cc
  src/core/debug.cc
  src/core/effective_config.cc
  src/core/env.cc
  src/core/math.cc
  src/core/events.cc
  src/core/file_watch.cc
  src/core/fs.cc
  src/core/platform_posix.cc
  src/core/signals.cc
  src/core/steering.cc
  src/core/strings.cc
  src/core/term.cc
  src/mcp/result.cc
  src/media/attachments.cc
  src/providers.cc
  src/tools/adapt_system.cc
  src/tools/child_agent.cc
  src/tools/files.cc
  src/tools/jobs.cc
  src/tools/memory.cc
  src/tools/memory_pipeline.cc
  src/tools/policy.cc
  src/tools/process_io.cc
  src/tools/configure.cc
  src/tools/registry.cc
  src/tools/self_info.cc
  src/tools/shell.cc
  src/tools/subagent.cc
  src/tools/web_fetch.cc
  src/tools/web_search.cc
  src/transport/sse.cc
  src/ui/conversation.cc
  src/ui/input_decoder.cc
  src/ui/interactive.cc
  src/ui/markdown.cc
  src/ui/presentation.cc)
target_link_libraries(uagent_core PUBLIC uagent_options)

add_executable(uagent src/main.cc)
target_link_libraries(uagent PRIVATE uagent_core)

# Link-time optimization has to cover the library: main.cc is a few lines and
# every code path lives in uagent_core.
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_ok OUTPUT _)
if(ipo_ok)
  set_property(TARGET uagent_core PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE
                                           TRUE)
  set_property(TARGET uagent PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(
    uagent_tests
    tests/unit/activity_sessions_test.cc
    tests/unit/attachment_encoding_test.cc
    tests/unit/config_proposal_test.cc
    tests/unit/config_registry_test.cc
    tests/unit/config_test.cc
    tests/unit/conversation_test.cc
    tests/unit/events_test.cc
    tests/unit/filesystem_test.cc
    tests/unit/math_test.cc
    tests/unit/grep_tool_test.cc
    tests/unit/input_decoder_test.cc
    tests/unit/mcp_session_test.cc
    tests/unit/openrouter_server_search_test.cc
    tests/unit/presentation_test.cc
    tests/unit/protocol_test.cc
    tests/unit/python_tool_test.cc
    tests/unit/runtime_test.cc
    tests/unit/skills_test.cc
    tests/unit/stream_test.cc
    tests/unit/test_main.cc
    tests/unit/tool_execution_policy_test.cc
    tests/unit/wire_test.cc)
  target_link_libraries(uagent_tests PRIVATE uagent_core)
  target_compile_definitions(
    uagent_tests PRIVATE UAGENT_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME core COMMAND uagent_tests)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  foreach(group IN ITEMS runtime tools ui providers mcp delegation)
    add_test(NAME integration_${group}
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py
                     $<TARGET_FILE:uagent> --group ${group})
    set_tests_properties(integration_${group} PROPERTIES TIMEOUT 180)
  endforeach()
  add_test(NAME session_metrics_self_test
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/session_metrics.py
                   --self-test)
  add_test(NAME eval_harness_self_test
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/eval.py
                   --self-test)
  # Behavioral scoreboard: every scenario is scored and compared against the
  # committed baseline, so a harness change that costs rounds, batching or
  # answer quality fails here rather than being noticed later.
  add_test(NAME behavior_eval
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/eval.py
                   $<TARGET_FILE:uagent> --check --timeout 30)
  # Each scenario enforces its own subprocess deadline. Leave enough wrapper
  # headroom for loaded hosted macOS runners to report that diagnostic.
  set_tests_properties(behavior_eval PROPERTIES TIMEOUT 120)
endif()

option(UAGENT_BUILD_BENCHMARKS "Build lightweight performance baselines" OFF)
if(UAGENT_BUILD_BENCHMARKS)
  add_executable(uagent_bench benchmarks/bench_core.cc)
  target_link_libraries(uagent_bench PRIVATE uagent_core)
  # Registered so the benchmark cannot silently stop compiling or running.
  if(BUILD_TESTING)
    add_test(NAME benchmarks COMMAND uagent_bench)
    set_tests_properties(benchmarks PROPERTIES TIMEOUT 180)
  endif()
endif()

if(UAGENT_BUILD_FUZZERS)
  foreach(_fuzzer sse input_decoder)
    add_executable(uagent_fuzz_${_fuzzer} tests/fuzz/fuzz_${_fuzzer}.cc)
    target_link_libraries(uagent_fuzz_${_fuzzer} PRIVATE uagent_core)
    target_link_options(uagent_fuzz_${_fuzzer} PRIVATE -fsanitize=fuzzer)
  endforeach()
endif()

install(TARGETS uagent RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(
  DIRECTORY skills/
  DESTINATION ${CMAKE_INSTALL_DATADIR}/uagent/skills
  PATTERN ".*" EXCLUDE
  PATTERN "__pycache__" EXCLUDE)
# The architecture narrative is authored once; the package carries a copy so
# the skill can answer design questions without a source checkout.
install(
  FILES docs/ARCHITECTURE.md
  DESTINATION ${CMAKE_INSTALL_DATADIR}/uagent/skills/uagent-config/references
  RENAME architecture.md)
set(UAGENT_GUIDES
    docs/ARCHITECTURE.md
    docs/OPERATIONS.md
    docs/PERSISTENCE.md
    docs/TESTING.md
    docs/TOOLS.md)
install(FILES README.md SECURITY.md CONTRIBUTING.md LICENSE ${UAGENT_GUIDES}
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
# Keep the source-relative paths used by README and CONTRIBUTING valid in an
# installed documentation tree while retaining the historical flat copies.
install(FILES ${UAGENT_GUIDES} DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs)
