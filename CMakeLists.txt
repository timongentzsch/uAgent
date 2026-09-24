cmake_minimum_required(VERSION 3.21)
project(uagent VERSION 1.0.0 DESCRIPTION "Lean terminal coding agent" LANGUAGES CXX)
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

# Layered static libs mirror the event-spine dependency DAG:
#   core_base (leaf: core/transport/media) <- api <- toolcore (tool
#   vocabulary + provider catalog: no agent, tool, or app dependency)
#   <- agent (turn loop, session persistence, supervision services:
#   process/jobs/child_agent, file writes, memory store, observation
#   records) <- tools (tool surface + mcp adapters, consuming agent
#   services) <- app (app/ui/cli, the consumer shell).
# Touching agent/ headers rebuilds agent+tools+app, not toolcore/api/core;
# touching tools/ headers rebuilds tools+app only.
add_library(
  uagent_core_base
  src/core/activity.cc
  src/core/capture.cc
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
  src/core/library.cc
  src/core/platform_posix.cc
  src/core/private_store.cc
  src/core/sandbox.cc
  src/core/sandbox_posix.cc
  src/core/signals.cc
  src/core/skills.cc
  src/core/steering.cc
  src/core/strings.cc
  src/core/term.cc
  src/transport/sse.cc
  src/media/attachments.cc)
target_link_libraries(uagent_core_base PUBLIC uagent_options)

add_library(
  uagent_api
  src/api/capabilities.cc
  src/api/client.cc
  src/api/exchange.cc
  src/api/openai_stream.cc
  src/api/wire_request.cc
  src/api/wire_stream.cc)
target_link_libraries(uagent_api PUBLIC uagent_core_base)

add_library(
  uagent_toolcore
  src/tools/tool.cc
  src/providers/selection.cc
  src/providers/catalog.cc
  src/providers/policy.cc)
target_link_libraries(uagent_toolcore PUBLIC uagent_api)

add_library(
  uagent_agent
  src/agent/activity_completion.cc
  src/agent/activity_log.cc
  src/agent/agent.cc
  src/agent/child_agent.cc
  src/agent/conversation.cc
  src/agent/dispatch.cc
  src/agent/file_services.cc
  src/agent/jobs.cc
  src/agent/memory_store.cc
  src/agent/prompt.cc
  src/agent/process_io.cc
  src/agent/prompt_profile.cc
  src/agent/request.cc
  src/agent/session_store.cc
  src/agent/session_view.cc
  src/agent/tool_loop.cc
  src/agent/tool_presentation.cc
  src/agent/trace.cc
  src/agent/turn.cc
  src/agent/turn_budgets.cc
  src/agent/turn_response.cc)
target_link_libraries(uagent_agent PUBLIC uagent_toolcore)

add_library(
  uagent_tools
  src/browser/browser.cc
  src/browser/runtime.cc
  src/mcp/discover.cc
  src/mcp/invoke.cc
  src/mcp/register.cc
  src/mcp/result.cc
  src/mcp/rpc.cc
  src/mcp/server.cc
  src/tools/adapt_system.cc
  src/tools/browser.cc
  src/tools/collaborator_runtime.cc
  src/tools/files.cc
  src/tools/image_result.cc
  src/tools/memory.cc
  src/tools/memory_pipeline.cc
  src/tools/policy.cc
  src/tools/configure.cc
  src/tools/registry.cc
  src/tools/registry_activity.cc
  src/tools/registry_exec.cc
  src/tools/registry_files.cc
  src/tools/registry_memory.cc
  src/tools/shell.cc
  src/tools/session.cc
  src/tools/subagent.cc
  src/tools/web_fetch.cc
  src/tools/web_search.cc)
target_link_libraries(uagent_tools PUBLIC uagent_agent)

add_library(
  uagent_app
  src/app/application.cc
  src/app/session_command.cc
  src/app/outcome_store.cc
  src/app/application_channel.cc
  src/app/application_headless.cc
  src/app/bootstrap.cc
  src/app/commands.cc
  src/app/commands_control.cc
  src/app/commands_model.cc
  src/app/commands_session.cc
  src/app/config_proposal.cc
  src/app/config_control.cc
  src/app/library.cc
  src/app/prompt_control.cc
  src/app/permissions.cc
  src/app/tool_categories.cc
  src/app/control.cc
  src/app/schedule.cc
  src/cli/options.cc
  src/cli/reference.cc
  src/app/runtime.cc
  src/app/self_description.cc
  src/app/asset_store.cc
  src/app/session_assets.cc
  src/app/session_host.cc
  src/app/session_router.cc
  src/app/replay_log.cc
  src/app/session_schedules.cc
  src/app/session_snapshot.cc
  src/app/session_supervisor.cc
  src/cli/cli.cc
  src/app/session_transport.cc
  src/app/session_server.cc
  src/app/session_worker.cc
  src/app/session_terminal.cc
  src/ui/conversation.cc
  src/ui/input_decoder.cc
  src/ui/interactive.cc
  src/ui/editor.cc
  src/ui/markdown.cc
  src/ui/presentation.cc)
target_link_libraries(uagent_app PUBLIC uagent_tools)

# Umbrella preserves the existing `uagent_core` link name for tests, benches,
# fuzzers and the web lib while the DAG above does the real work.
add_library(uagent_core INTERFACE)
target_link_libraries(uagent_core INTERFACE uagent_app)

add_executable(uagent src/main.cc)
option(UAGENT_WEB "Embed the native web master and PWA" ON)
option(UAGENT_WEB_PUSH "Build the optional OpenSSL Web Push sender" OFF)
if(UAGENT_WEB)
  include(cmake/embed_web.cmake)
  add_library(uagent_web src/web/master.cc src/web/browser_viewer.cc
                         src/web/rfb_filter.cc src/web/push.cc
                         "${_web_generated}")
  target_link_libraries(uagent_web PUBLIC uagent_core)
  target_include_directories(uagent_web SYSTEM PRIVATE third_party/cpp-httplib)
  target_compile_definitions(uagent PRIVATE UAGENT_WEB)
  target_link_libraries(uagent PRIVATE uagent_web)
  if(UAGENT_WEB_PUSH)
    find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
    target_sources(uagent_web PRIVATE src/web/push_crypto.cc)
    target_compile_definitions(uagent_web PRIVATE UAGENT_WEB_PUSH)
    target_link_libraries(uagent_web PUBLIC OpenSSL::Crypto)
  endif()
elseif(UAGENT_WEB_PUSH)
  message(FATAL_ERROR "UAGENT_WEB_PUSH requires UAGENT_WEB")
else()
  target_link_libraries(uagent PRIVATE uagent_core)
endif()

# `third_party/json.hpp` is 25k lines and reaches 82 of the ~90 translation
# units through include/core/json.h. Measured on an M-series host it costs
# 0.75s per TU over an empty one -- roughly a third of a clean build. It is
# vendored and never changes, so a precompiled header is stable, needs no
# source change, and takes about 20% off every TU that rebuilds. This does not
# reduce rebuild fanout; it reduces what each of those rebuilds costs.
set(UAGENT_PCH
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/json.hpp"
    <algorithm>
    <chrono>
    <cstdint>
    <optional>
    <string>
    <string_view>
    <utility>
    <vector>)
foreach(_uagent_lib IN ITEMS uagent_core_base uagent_api uagent_toolcore uagent_agent uagent_tools uagent_app)
  target_precompile_headers(${_uagent_lib} PRIVATE ${UAGENT_PCH})
endforeach()

# Link-time optimization has to cover the libraries: main.cc is a few lines
# and every code path lives in the layered static libs behind uagent_core.
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_ok OUTPUT _)
if(ipo_ok)
  foreach(_uagent_lib IN ITEMS uagent_core_base uagent_api uagent_toolcore uagent_agent uagent_tools uagent_app)
    set_property(TARGET ${_uagent_lib} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  endforeach()
  set_property(TARGET uagent PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  if(UAGENT_WEB)
    set_property(TARGET uagent_web PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  endif()
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(
    uagent_tests
    tests/unit/browser_test.cc
    src/web/rfb_filter.cc
    tests/unit/activity_sessions_test.cc
    tests/unit/attachment_encoding_test.cc
    tests/unit/config_proposal_test.cc
    tests/unit/config_coherence_test.cc
    tests/unit/config_registry_test.cc
    tests/unit/config_test.cc
    tests/unit/conversation_test.cc
    tests/unit/events_test.cc
    tests/unit/filesystem_test.cc
    tests/unit/math_test.cc
    tests/unit/grep_tool_test.cc
    tests/unit/input_decoder_test.cc
    tests/unit/memory_test.cc
    tests/unit/mcp_session_test.cc
    tests/unit/openrouter_server_search_test.cc
    tests/unit/presentation_test.cc
    tests/unit/protocol_test.cc
    tests/unit/prompt_test.cc
    tests/unit/python_tool_test.cc
    tests/unit/runtime_test.cc
    tests/unit/sandbox_test.cc
    tests/unit/session_command_test.cc
    tests/unit/session_server_test.cc
    tests/unit/skills_test.cc
    tests/unit/stream_test.cc
    tests/unit/test_main.cc
    tests/unit/tool_execution_policy_test.cc
    tests/unit/wire_test.cc)
  target_link_libraries(uagent_tests PRIVATE uagent_core)
  # Its own PCH rather than REUSE_FROM: the test binary carries an extra -D,
  # and a reused header has to have been built with the same command line.
  target_precompile_headers(uagent_tests PRIVATE ${UAGENT_PCH})
  target_compile_definitions(
    uagent_tests PRIVATE UAGENT_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME core COMMAND uagent_tests)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  if(UAGENT_WEB)
    add_test(NAME integration_web COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py $<TARGET_FILE:uagent> --group web)
    set_tests_properties(integration_web PROPERTIES TIMEOUT 180)
    add_test(NAME integration_management COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py $<TARGET_FILE:uagent> --group management)
    set_tests_properties(integration_management PROPERTIES TIMEOUT 120)
    if(UAGENT_WEB_PUSH)
      add_executable(uagent_push_tests tests/unit/web_push_test.cc)
      target_link_libraries(uagent_push_tests PRIVATE uagent_web)
      add_test(NAME web_push COMMAND uagent_push_tests)
    endif()
  endif()
  foreach(group IN ITEMS runtime tools ui providers mcp delegation sandbox)
    add_test(NAME integration_${group}
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py
                     $<TARGET_FILE:uagent> --group ${group})
    # A backstop, not the deadline. Every case inside the runner carries its
    # own budget, scaled by UAGENT_TEST_TIMEOUT_SCALE, and that budget is what
    # should fail -- it prints the captured terminal session and names the
    # case. This ceiling is not scaled, so at the sanitizer legs' scale of 6 a
    # single 30s case reached exactly 180s and ctest killed the runner first,
    # reporting a timeout with an empty log. Set above the slowest real group
    # (TSan runtime, ~5min) so the runner always wins the race.
    set_tests_properties(integration_${group} PROPERTIES TIMEOUT 900)
  endforeach()
  add_test(NAME ci_changes
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/ci_changes_test.py)
  set_tests_properties(ci_changes PROPERTIES LABELS source)
  add_test(NAME layer_boundary
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/boundary_test.py)
  set_tests_properties(layer_boundary PROPERTIES LABELS source)
  add_test(NAME wire_contract
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/wire_contract_test.py)
  set_tests_properties(wire_contract PROPERTIES LABELS source)
  add_test(NAME session_metrics_self_test
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/session_metrics.py
                   --self-test)
  add_test(NAME eval_harness_self_test
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/eval.py
                   --self-test)
  set_tests_properties(session_metrics_self_test eval_harness_self_test PROPERTIES LABELS source)
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
    docs/CACHING.md
    docs/OPERATIONS.md
    docs/PERSISTENCE.md
    docs/TESTING.md
    docs/TOOLS.md
    docs/WEB.md
    docs/SYSTEM_PROMPTS.md
    docs/MANAGEMENT.md)
install(FILES README.md SECURITY.md CONTRIBUTING.md LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
# Keep the source-relative paths used by README and CONTRIBUTING valid in an
# installed documentation tree.
install(FILES ${UAGENT_GUIDES} DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs)
install(FILES benchmarks/baselines/web.json
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/benchmarks/baselines)
if(UAGENT_WEB)
  install(FILES web/THIRD_PARTY_NOTICES.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
  install(FILES third_party/cpp-httplib/LICENSE
          DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses RENAME cpp-httplib.txt)
endif()
