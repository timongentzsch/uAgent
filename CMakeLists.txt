cmake_minimum_required(VERSION 3.21)
project(uagent VERSION 1.9.0 DESCRIPTION "Lean terminal coding agent" LANGUAGES CXX)
include(GNUInstallDirs)

# The build id hashes the runtime's inputs, not HEAD, so an unpublished edit
# never attaches to an older worker. Frontend assets stay out of it: the
# normal and the remote flavor must agree.
file(GLOB_RECURSE _runtime_inputs CONFIGURE_DEPENDS
     RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*"
     "${CMAKE_CURRENT_SOURCE_DIR}/include/*"
     "${CMAKE_CURRENT_SOURCE_DIR}/third_party/*"
     "${CMAKE_CURRENT_SOURCE_DIR}/skills/*")
list(FILTER _runtime_inputs EXCLUDE REGEX "(^|/)([.]|__pycache__)|[.]pyc$")
list(APPEND _runtime_inputs "CMakeLists.txt"
     "cmake/embed_web.cmake"
     "cmake/build_identity.cc.in"
     "web/public/design-helper.js"
     "docs/ARCHITECTURE.md"
     "web/src/shared/styles/tokens.css")
list(SORT _runtime_inputs)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_runtime_inputs})
set(_runtime_hashes "${PROJECT_VERSION}")
foreach(_input IN LISTS _runtime_inputs)
  file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${_input}" _hash)
  string(APPEND _runtime_hashes "${_input}:${_hash}\n")
endforeach()
string(SHA256 UAGENT_BUILD_ID "${_runtime_hashes}")
configure_file(cmake/build_identity.cc.in build_identity.cc @ONLY)

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

find_package(CURL 7.66 REQUIRED)
find_package(Threads REQUIRED)

add_library(uagent_options INTERFACE)
target_include_directories(uagent_options INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(uagent_options INTERFACE CURL::libcurl Threads::Threads)
target_compile_features(uagent_options INTERFACE cxx_std_20)
target_compile_definitions(uagent_options INTERFACE UAGENT_VERSION="${PROJECT_VERSION}")
if(_IS_GCC_CLANG)
  target_compile_options(
    uagent_options INTERFACE -fno-exceptions -Wall -Wextra -Wpedantic
                             -Wconversion -Wsign-conversion -Wshadow
                             -Wold-style-cast)
endif()
if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 20)
  # Xcode 16 ships stop_token and jthread behind this libc++ gate.
  target_compile_options(uagent_options INTERFACE -fexperimental-library)
endif()
option(UAGENT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
if(UAGENT_WARNINGS_AS_ERRORS AND _IS_GCC_CLANG)
  target_compile_options(uagent_options INTERFACE -Werror)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # Only libc++ annotates std::mutex, so the analysis needs Clang with it.
  include(CheckCXXSourceCompiles)
  check_cxx_source_compiles(
    "#include <version>
#ifndef _LIBCPP_VERSION
#error not libc++
#endif
int main() {}"
    UAGENT_USING_LIBCPP)
  if(UAGENT_USING_LIBCPP)
    target_compile_definitions(
      uagent_options INTERFACE UAGENT_ENABLE_THREAD_SAFETY_ANALYSIS
                               _LIBCPP_ENABLE_THREAD_SAFETY_ANNOTATIONS)
    target_compile_options(uagent_options INTERFACE -Wthread-safety)
  endif()
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

# Static libraries in layers, each depending only on those before it:
# core_base <- api <- toolcore <- agent <- tools <- app.
add_library(
  uagent_core_base
  "${CMAKE_CURRENT_BINARY_DIR}/build_identity.cc"
  src/core/turn_status.cc
  src/core/capture.cc
  src/core/child_env.cc
  src/core/config.cc
  src/core/config_registry.cc
  src/core/debug.cc
  src/core/effective_config.cc
  src/core/env.cc
  src/core/math.cc
  src/core/events.cc
  src/core/file_watch.cc
  src/core/fs.cc
  src/core/workspace.cc
  src/core/library.cc
  src/core/mailbox.cc
  src/core/platform_posix.cc
  src/core/private_store.cc
  src/core/project.cc
  src/core/sandbox.cc
  src/core/sandbox_posix.cc
  src/core/sandbox_watch.cc
  src/core/settings_store.cc
  src/core/signals.cc
  src/core/skills.cc
  src/core/steering.cc
  src/core/strings.cc
  src/core/style.cc
  src/core/term.cc
  src/core/usage.cc
  src/transport/session.cc
  src/transport/sse.cc)
target_link_libraries(uagent_core_base PUBLIC uagent_options)

add_library(
  uagent_api
  src/api/capabilities.cc
  src/api/client.cc
  src/api/exchange.cc
  src/api/wire_request.cc
  src/api/wire_stream.cc)
target_link_libraries(uagent_api PUBLIC uagent_core_base)

add_library(
  uagent_toolcore
  src/media/attachments.cc
  src/tools/tool.cc
  src/providers/selection.cc
  src/providers/catalog.cc
  src/providers/policy.cc)
target_link_libraries(uagent_toolcore PUBLIC uagent_api)

add_library(
  uagent_agent
  src/agent/agent.cc
  src/agent/activity_drain.cc
  src/agent/child_agent.cc
  src/agent/mail_delivery.cc
  src/agent/compaction.cc
  src/agent/conversation.cc
  src/agent/dispatch.cc
  src/agent/edit_journal.cc
  src/agent/file_services.cc
  src/agent/activity.cc
  src/agent/memory_store.cc
  src/agent/prompt.cc
  src/agent/activity_supervisor.cc
  src/agent/prompt_profile.cc
  src/agent/request.cc
  src/agent/session_links.cc
  src/agent/session_store.cc
  src/agent/session_search.cc
  src/agent/transcript_query.cc
  src/agent/transcript_tree.cc
  src/agent/view_reducer.cc
  src/agent/tool_loop.cc
  src/agent/trace.cc
  src/agent/turn.cc
  src/agent/turn_budgets.cc
  src/agent/turn_response.cc
  src/agent/usage_accounting.cc)
target_link_libraries(uagent_agent PUBLIC uagent_toolcore)

add_library(
  uagent_tools
  src/mcp/discover.cc
  src/mcp/invoke.cc
  src/mcp/register.cc
  src/mcp/rpc.cc
  src/mcp/server.cc
  src/tools/ask.cc
  src/tools/files.cc
  src/tools/image_result.cc
  src/tools/memory.cc
  src/tools/memory_pipeline.cc
  src/tools/policy.cc
  src/tools/registry.cc
  src/tools/registry_activity.cc
  src/tools/registry_exec.cc
  src/tools/registry_files.cc
  src/tools/registry_memory.cc
  src/tools/registry_todo.cc
  src/tools/shell.cc
  src/tools/web_fetch.cc
  src/tools/web_search.cc)
target_link_libraries(uagent_tools PUBLIC uagent_agent)

add_library(
  uagent_app
  src/app/terminal.cc
  src/app/workspace_files.cc
  src/app/execution_wire.cc
  src/app/execution.cc
  src/app/execution_client.cc
  src/app/application.cc
  src/app/session_command.cc
  src/app/outcome_store.cc
  src/app/application_channel.cc
  src/app/application_headless.cc
  src/app/bootstrap.cc
  src/app/bootstrap_tools.cc
  src/app/approvals.cc
  src/app/commands.cc
  src/app/commands_control.cc
  src/app/commands_model.cc
  src/app/commands_session.cc
  src/app/config_proposal.cc
  src/app/config_control.cc
  src/app/import.cc
  src/app/library.cc
  src/app/permissions.cc
  src/app/control.cc
  src/app/schedule.cc
  src/app/launch.cc
  src/app/folder_overview.cc
  src/app/agent_link.cc
  src/app/agent_tools.cc
  src/app/uagent_tool.cc
  src/cli/options.cc
  src/app/runtime.cc
  src/app/self_description.cc
  src/app/present.cc
  src/app/asset_store.cc
  src/app/session_assets.cc
  src/app/session_host.cc
  src/app/session_move.cc
  src/app/session_router.cc
  src/app/replay_log.cc
  src/app/session_schedules.cc
  src/app/session_snapshot.cc
  src/app/session_supervisor.cc
  src/cli/cli.cc
  src/app/session_server.cc
  src/app/session_worker.cc
  src/app/setup.cc
  src/app/transient_batcher.cc
  src/app/session_terminal.cc
  src/app/session_terminal_commands.cc
  src/app/session_terminal_link.cc
  src/app/session_terminal_screen.cc
  src/ui/input_decoder.cc
  src/ui/ask_picker.cc
  src/ui/interactive.cc
  src/ui/frame.cc
  src/ui/completion.cc
  src/ui/composer.cc
  src/ui/live_region.cc
  src/ui/editor.cc
  src/ui/markdown.cc
  src/ui/markdown_math.cc
  src/ui/markdown_table.cc
  src/ui/presentation.cc
  src/ui/tree_render.cc)
target_link_libraries(uagent_app PUBLIC uagent_tools)

add_executable(uagent src/main.cc)
# A built binary finds the sheet engine beside it, as an installed one does.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/web/dist/share/sheet")
  add_custom_command(
    TARGET uagent POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/web/dist/share/sheet"
            "$<TARGET_FILE_DIR:uagent>/share/uagent/sheet")
endif()
# The web UI is built by `npm run build` in web/, not checked in.
option(UAGENT_WEB_PUSH "Build the optional OpenSSL Web Push sender" OFF)
include(cmake/embed_web.cmake)
add_library(uagent_web src/web/master.cc
                      src/web/remote_hosts.cc src/web/master_remote.cc
                      src/web/master_remote_terminal.cc src/web/master_auth.cc
                       src/web/master_command.cc src/web/master_files.cc
                       src/web/master_library.cc
                       src/web/master_stream.cc src/web/master_terminal.cc
                       src/web/browser_viewer.cc
                       src/web/rfb_filter.cc "${_web_generated}")
find_package(ZLIB REQUIRED)  # libcurl's own dependency: it inflates assets
target_link_libraries(uagent_web PUBLIC uagent_app ZLIB::ZLIB)
target_include_directories(uagent_web SYSTEM PRIVATE third_party/cpp-httplib)
target_sources(uagent_tools PRIVATE src/browser/browser.cc
                                    src/browser/broker.cc
                                    src/browser/runtime.cc
                                    src/browser/runtime_act.cc
                                    src/browser/runtime_control.cc
                                    src/browser/runtime_page.cc
                                    src/tools/browser.cc)
target_link_libraries(uagent PRIVATE uagent_web)
if(UAGENT_WEB_PUSH)
  find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
  target_sources(uagent_web PRIVATE src/web/push.cc src/web/push_crypto.cc)
  target_link_libraries(uagent_web PUBLIC OpenSSL::Crypto)
else()
  target_sources(uagent_web PRIVATE src/web/no_push.cc)
endif()

# json.hpp reaches nearly every translation unit and never changes.
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
set(UAGENT_LIBRARIES uagent_core_base uagent_api uagent_toolcore uagent_agent
                     uagent_tools uagent_app)
foreach(_uagent_lib IN LISTS UAGENT_LIBRARIES)
  target_precompile_headers(${_uagent_lib} PRIVATE ${UAGENT_PCH})
endforeach()

# The code lives in the static libraries, so LTO has to cover them.
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_ok OUTPUT _)
if(ipo_ok)
  foreach(_uagent_lib IN LISTS UAGENT_LIBRARIES)
    set_property(TARGET ${_uagent_lib} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  endforeach()
  set_property(TARGET uagent uagent_web PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
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
    tests/unit/transcript_tree_test.cc
    tests/unit/events_test.cc
    tests/unit/filesystem_test.cc
    tests/unit/math_test.cc
    tests/unit/grep_tool_test.cc
    tests/unit/input_decoder_test.cc
    tests/unit/memory_test.cc
    tests/unit/mcp_session_test.cc
    tests/unit/openrouter_server_search_test.cc
    tests/unit/permission_review_test.cc
    tests/unit/presentation_test.cc
    tests/unit/protocol_test.cc
    tests/unit/prompt_test.cc
    tests/unit/runtime_test.cc
    tests/unit/sandbox_test.cc
    tests/unit/scratch_test.cc
    tests/unit/session_command_test.cc
    tests/unit/session_server_test.cc
    tests/unit/settings_store_test.cc
    tests/unit/setup_test.cc
    tests/unit/skills_test.cc
    tests/unit/stream_test.cc
    tests/unit/test_main.cc
    tests/unit/tool_execution_policy_test.cc
    tests/unit/wire_test.cc)
  # uagent_web brings uagent_app; linking both lists it twice.
  target_sources(uagent_tests PRIVATE tests/unit/web_terminal_test.cc
                                      tests/unit/web_remote_test.cc
                                      tests/unit/browser_test.cc)
  target_include_directories(uagent_tests SYSTEM PRIVATE third_party/cpp-httplib)
  if(UAGENT_WEB_PUSH)
    target_sources(uagent_tests PRIVATE tests/unit/web_push_test.cc)
  endif()
  target_link_libraries(uagent_tests PRIVATE uagent_web)
  # Not REUSE_FROM: the test binary is built with an extra -D.
  target_precompile_headers(uagent_tests PRIVATE ${UAGENT_PCH})
  target_compile_definitions(
    uagent_tests PRIVATE UAGENT_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME core COMMAND uagent_tests)
  # A case that never returns fails here, not at the job's limit.
  set_tests_properties(core PROPERTIES TIMEOUT 600)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(_groups runtime tools ui providers mcp agents sandbox web management)
  foreach(group IN LISTS _groups)
    # The longest group by far: dealt to three runners.
    set(_runners)
    if(group STREQUAL "runtime")
      set(_runners -j3)
    endif()
    add_test(NAME integration_${group}
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py
                     $<TARGET_FILE:uagent> --group ${group} ${_runners})
    # A backstop above the slowest group: a case's own budget must fail
    # first, since only that names the case and prints its output.
    set_tests_properties(integration_${group} PROPERTIES TIMEOUT 900)
  endforeach()
  set_tests_properties(integration_runtime PROPERTIES PROCESSORS 3)
  # Scores every scenario against the committed baseline.
  add_test(NAME behavior_eval
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/eval.py
                   $<TARGET_FILE:uagent> --check --timeout 30)
  set_tests_properties(behavior_eval PROPERTIES TIMEOUT 120)
endif()

option(UAGENT_BUILD_BENCHMARKS "Build lightweight performance baselines" OFF)
if(UAGENT_BUILD_BENCHMARKS)
  add_executable(uagent_bench benchmarks/bench_core.cc)
  target_link_libraries(uagent_bench PRIVATE uagent_app)
  # Registered so the benchmark keeps compiling and running.
  if(BUILD_TESTING)
    add_test(NAME benchmarks COMMAND uagent_bench)
    set_tests_properties(benchmarks PROPERTIES TIMEOUT 180)
  endif()
endif()

if(UAGENT_BUILD_FUZZERS)
  # Each is built from what it fuzzes and the base library only.
  add_executable(uagent_fuzz_sse tests/fuzz/fuzz_sse.cc)
  add_executable(uagent_fuzz_input_decoder tests/fuzz/fuzz_input_decoder.cc
                                           src/ui/input_decoder.cc)
  foreach(_fuzzer sse input_decoder)
    target_link_libraries(uagent_fuzz_${_fuzzer} PRIVATE uagent_core_base)
    target_link_options(uagent_fuzz_${_fuzzer} PRIVATE -fsanitize=fuzzer)
  endforeach()
endif()

install(TARGETS uagent RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(
  DIRECTORY skills/
  DESTINATION ${CMAKE_INSTALL_DATADIR}/uagent/skills
  PATTERN ".*" EXCLUDE
  PATTERN "__pycache__" EXCLUDE)
# A sheet page fetches the engine and its drawing code: installed, not
# embedded.
install(DIRECTORY web/dist/share/sheet/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/uagent/sheet OPTIONAL)
# The skill answers design questions from this copy.
install(
  FILES docs/ARCHITECTURE.md
  DESTINATION ${CMAKE_INSTALL_DATADIR}/uagent/skills/uagent-config/references
  RENAME architecture.md)
# The web UI's design tokens, for the skill to read.
install(
  FILES web/src/shared/styles/tokens.css
  DESTINATION ${CMAKE_INSTALL_DATADIR}/uagent/skills/uagent-config/references)
# Every guide ships; tests/package_contents.py checks the same directory.
file(GLOB UAGENT_GUIDES CONFIGURE_DEPENDS docs/*.md)
install(FILES README.md SECURITY.md CONTRIBUTING.md LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
# README and CONTRIBUTING link to docs/ by relative path.
install(FILES ${UAGENT_GUIDES} DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs)
install(FILES web/dist/THIRD_PARTY_NOTICES.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES third_party/cpp-httplib/LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses RENAME cpp-httplib.txt)
