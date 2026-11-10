# Validate before publishing: an incomplete developer build never claims to be
# a remote-ready installation. The caller has already built BINARY.
execute_process(COMMAND "${BINARY}" --build-id OUTPUT_VARIABLE build
                OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
string(LENGTH "${build}" length)
if(NOT length EQUAL 64 OR NOT build MATCHES "^[0-9a-f]+$")
  message(FATAL_ERROR "invalid runtime build identity")
endif()
if(NOT EXISTS "${BUNDLE}/manifest.json")
  message(FATAL_ERROR "No complete matching remote bundle. Set UAGENT_REMOTE_BUNDLE to its directory, or UAGENT_LOCAL_ONLY=1 for an explicit local-only developer install.")
endif()
execute_process(COMMAND "${BINARY}" --check-remote-bundle "${BUNDLE}"
                COMMAND_ERROR_IS_FATAL ANY)
set(files manifest.json linux-x86_64.tar.gz linux-arm64.tar.gz macos-x86_64.tar.gz macos-arm64.tar.gz)
if(NOT DEFINED DESTINATION)
  return()
endif()
set(final "${DESTINATION}/${build}")
if(EXISTS "${final}/manifest.json")
  execute_process(COMMAND "${CMAKE_COMMAND}" "-DBINARY=${BINARY}"
                  "-DBUNDLE=${final}" -P "${CMAKE_CURRENT_LIST_FILE}"
                  COMMAND_ERROR_IS_FATAL ANY)
  return()
endif()
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef suffix)
set(stage "${DESTINATION}/${build}.new-${suffix}")
file(MAKE_DIRECTORY "${stage}")
foreach(file IN LISTS files)
  file(COPY_FILE "${BUNDLE}/${file}" "${stage}/${file}")
endforeach()
# Recheck copied bytes, so an interrupted/concurrent source update publishes
# nothing. Recursive invocation has no DESTINATION and only validates.
execute_process(COMMAND "${CMAKE_COMMAND}" "-DBINARY=${BINARY}"
                "-DBUNDLE=${stage}" -P "${CMAKE_CURRENT_LIST_FILE}"
                RESULT_VARIABLE checked)
if(NOT checked EQUAL 0)
  file(REMOVE_RECURSE "${stage}")
  message(FATAL_ERROR "remote bundle copy failed verification")
endif()
file(RENAME "${stage}" "${final}" RESULT published)
if(NOT published STREQUAL "0")
  file(REMOVE_RECURSE "${stage}")
  message(FATAL_ERROR "cannot publish remote bundle: ${published}")
endif()
