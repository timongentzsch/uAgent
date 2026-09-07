// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_TESTS_UNIT_TERMINAL_TEST_SUPPORT_H_
#define UAGENT_TESTS_UNIT_TERMINAL_TEST_SUPPORT_H_
#include <cstdio>
#include <string>
#include <utility>

#include "include/core/term.h"
#include "tests/unit/test_support.h"
namespace uagent {
template <typename Writer>
std::string CaptureStdout(Writer&& writer, bool color = false,
                          bool tty = true) {
  fflush(stdout);
  int saved = dup(STDOUT_FILENO);
  FILE* capture = tmpfile();
  if (saved < 0 || !capture) {
    if (saved >= 0) close(saved);
    if (capture) fclose(capture);
    return {};
  }
  dup2(fileno(capture), STDOUT_FILENO);
  bool prior_tty = g_tty;
  bool prior_color = g_color;
  g_tty = tty;
  g_color = color;
  std::forward<Writer>(writer)();
  fflush(stdout);
  g_tty = prior_tty;
  g_color = prior_color;
  dup2(saved, STDOUT_FILENO);
  close(saved);
  fseek(capture, 0, SEEK_END);
  const int64_t bytes = static_cast<int64_t>(ftell(capture));
  fseek(capture, 0, SEEK_SET);
  std::string output(bytes > 0 ? static_cast<size_t>(bytes) : 0, '\0');
  if (!output.empty()) {
    // Short reads are the caller's business: resizing to what arrived keeps
    // the captured text honest instead of padding it with the NULs above.
    output.resize(fread(output.data(), 1, output.size(), capture));
  }
  fclose(capture);
  return output;
}

}  // namespace uagent
#endif
