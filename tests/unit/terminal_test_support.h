// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_TESTS_UNIT_TERMINAL_TEST_SUPPORT_H_
#define UAGENT_TESTS_UNIT_TERMINAL_TEST_SUPPORT_H_
#include <unistd.h>

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
    // A short read must not leave NUL padding in the captured text.
    output.resize(fread(output.data(), 1, output.size(), capture));
  }
  fclose(capture);
  return output;
}

// TerminalColumns() probes TIOCGWINSZ on both standard descriptors before
// COLUMNS, so neither may be a terminal for the width to be known.
class FixedWidth {
 public:
  explicit FixedWidth(int columns) {
    const char* prior = getenv("COLUMNS");
    had_columns_ = prior != nullptr;
    if (prior) saved_columns_ = prior;
    setenv("COLUMNS", std::to_string(columns).c_str(), 1);
    empty_ = tmpfile();
    if (empty_) {
      saved_out_ = dup(STDOUT_FILENO);
      saved_in_ = dup(STDIN_FILENO);
      dup2(fileno(empty_), STDOUT_FILENO);
      dup2(fileno(empty_), STDIN_FILENO);
    }
  }
  ~FixedWidth() {
    if (saved_out_ >= 0) {
      dup2(saved_out_, STDOUT_FILENO);
      close(saved_out_);
    }
    if (saved_in_ >= 0) {
      dup2(saved_in_, STDIN_FILENO);
      close(saved_in_);
    }
    if (empty_) fclose(empty_);
    if (had_columns_) {
      setenv("COLUMNS", saved_columns_.c_str(), 1);
    } else {
      unsetenv("COLUMNS");
    }
  }
  FixedWidth(const FixedWidth&) = delete;
  FixedWidth& operator=(const FixedWidth&) = delete;

 private:
  bool had_columns_ = false;
  std::string saved_columns_;
  FILE* empty_ = nullptr;
  int saved_out_ = -1;
  int saved_in_ = -1;
};

}  // namespace uagent
#endif
