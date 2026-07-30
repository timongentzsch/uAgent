// Copyright 2026 Timon Gentzsch

#include <string>
#include <vector>

#include "tests/unit/test_support.h"

namespace uagent {

void TestFileTools() {
  namespace fs = std::filesystem;
  auto contents = [](const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
  };
  fs::path root =
      fs::temp_directory_path() /
      ("uagent-test-" + std::to_string(static_cast<int64_t>(getpid())));
  fs::create_directories(root);
  ToolResult missing_read =
      ToolReadFile((root / "missing.txt").string(), int64_t{1}, int64_t{1});
  CHECK(!missing_read.Ok());
  CHECK(missing_read.error == ToolErrorCode::kNotFound);
  CHECK(ToolReadFile((root / "missing.txt").string(), 1, 1).output ==
        missing_read.output);
  ToolResult missing_list = ToolListDir((root / "missing-dir").string());
  CHECK(!missing_list.Ok());
  CHECK(missing_list.error == ToolErrorCode::kNotFound);
  fs::path small_directory = root / "small-directory";
  fs::create_directories(small_directory);
  CHECK(ToolWriteFile((small_directory / "one.cc").string(), "ONE_BODY\n")
            .output.starts_with("wrote "));
  CHECK(ToolWriteFile((small_directory / "two.cc").string(), "TWO_BODY\n")
            .output.starts_with("wrote "));
  ToolResult names_only = ToolListDir(small_directory.string(), 0, 0, false);
  CHECK(names_only.output.find("one.cc") != std::string::npos);
  CHECK(names_only.output.find("ONE_BODY") == std::string::npos);
  ToolResult preview = ToolListDir(small_directory.string(), 0, 0, true);
  CHECK(preview.output.find("ONE_BODY") != std::string::npos);
  CHECK(preview.output.find("TWO_BODY") != std::string::npos);
  CHECK(preview.result_chars == ReadFileResultChars());
  fs::create_directories(small_directory / "nested");
  CHECK(ToolListDir(small_directory.string(), 0, 0, true)
            .output.find("ONE_BODY") == std::string::npos);
  fs::path binary_directory = root / "binary-directory";
  fs::create_directories(binary_directory);
  CHECK(ToolWriteFile((binary_directory / "data.bin").string(),
                      std::string("text\0binary", 11))
            .output.starts_with("wrote "));
  CHECK(ToolListDir(binary_directory.string(), 0, 0, true)
            .output.find("small directory contents") == std::string::npos);
  fs::path symlink_directory = root / "symlink-directory";
  fs::create_directories(symlink_directory);
  fs::create_symlink(small_directory / "one.cc",
                     symlink_directory / "linked.cc");
  CHECK(ToolListDir(symlink_directory.string(), 0, 0, true)
            .output.find("ONE_BODY") == std::string::npos);
  fs::path file = root / "file.txt";
  CHECK(
      ToolWriteFile(file.string(), "one\ntwo\n").output.starts_with("wrote "));
  std::string read = ToolReadFile(file.string(), 1, 1).output;
  CHECK(read.find("lines 1-1") != std::string::npos);
  CHECK(read.find("\none\n") != std::string::npos);
  fs::path long_line = root / "long-line.txt";
  CHECK(ToolWriteFile(long_line.string(), std::string(40 * 1024, 'x'))
            .output.starts_with("wrote "));
  std::string long_read = ToolReadFile(long_line.string(), 1, 1).output;
  CHECK(long_read.find("lines 1-1; line prefix limited") != std::string::npos);
  CHECK(long_read.find(std::string(1024, 'x')) != std::string::npos);
  fs::path ordinary_source = root / "ordinary-source.txt";
  std::string ordinary_source_text;
  for (int line = 1; line <= 300; ++line) {
    ordinary_source_text += "line " + std::to_string(line) + "\n";
  }
  CHECK(ToolWriteFile(ordinary_source.string(), ordinary_source_text)
            .output.starts_with("wrote "));
  std::string ordinary_read =
      ToolReadFile(ordinary_source.string(), 1, 0).output;
  CHECK(ordinary_read.find("line 300\n") != std::string::npos);
  CHECK(ordinary_read.find("more available") == std::string::npos);
  CHECK(ToolEditFile(file.string(), "two", "three")
            .output.starts_with("edited "));
  ToolResult missing_edit =
      ToolEditFile(file.string(), "missing", "replacement");
  CHECK(!missing_edit.Ok());
  CHECK(missing_edit.error == ToolErrorCode::kNotFound);
  ToolResult empty_edit = ToolEditFile(file.string(), std::vector<FileEdit>{});
  CHECK(!empty_edit.Ok());
  CHECK(empty_edit.error == ToolErrorCode::kInvalidArguments);

  struct stat before{}, after{};
  CHECK(stat(file.c_str(), &before) == 0);
  CHECK(ToolEditFile(file.string(), "three", "three")
            .output.find("makes no change") != std::string::npos);
  CHECK(stat(file.c_str(), &after) == 0);
  CHECK(before.st_ino == after.st_ino);

  fs::path crlf = root / "crlf.txt";
  CHECK(ToolWriteFile(crlf.string(), "one\r\ntwo\r\n")
            .output.starts_with("wrote "));
  CHECK(ToolEditFile(crlf.string(), "one\ntwo\n", "ONE\ntwo\n")
            .output.starts_with("edited "));
  CHECK(contents(crlf) == "ONE\r\ntwo\r\n");
  CHECK(ToolEditFile(crlf.string(), "two", "two\nthree")
            .output.starts_with("edited "));
  CHECK(contents(crlf) == "ONE\r\ntwo\r\nthree\r\n");
  CHECK(ToolEditFile(file.string(), "one\r\nthree\r\n", "ONE\r\nthree\r\n")
            .output.starts_with("edited "));
  CHECK(contents(file) == "ONE\nthree\n");

  fs::path mixed = root / "mixed.txt";
  CHECK(ToolWriteFile(mixed.string(), "a\r\nb\nc\n")
            .output.starts_with("wrote "));
  CHECK(
      ToolEditFile(mixed.string(), "c", "x\ny").output.starts_with("edited "));
  CHECK(contents(mixed) == "a\r\nb\nx\ny\n");

  fs::path literal_crlf = root / "literal-crlf.txt";
  CHECK(ToolWriteFile(literal_crlf.string(), "payload\n")
            .output.starts_with("wrote "));
  CHECK(ToolEditFile(literal_crlf.string(), "payload", "a\r\nb")
            .output.starts_with("edited "));
  CHECK(contents(literal_crlf) == "a\r\nb\n");

  fs::path bom = root / "bom.txt";
  CHECK(ToolWriteFile(bom.string(), "\xEF\xBB\xBFone\n")
            .output.starts_with("wrote "));
  CHECK(ToolEditFile(bom.string(), "one", "two").output.starts_with("edited "));
  CHECK(contents(bom) == "\xEF\xBB\xBFtwo\n");

  fs::path batch = root / "batch.txt";
  CHECK(ToolWriteFile(batch.string(), "alpha one alpha two\n")
            .output.starts_with("wrote "));
  std::string batch_result =
      ToolEditFile(batch.string(),
                   {{"alpha", "beta", true}, {"beta two", "gamma", false}})
          .output;
  CHECK(batch_result.find("3 replacements across 2 edits") !=
        std::string::npos);
  CHECK(contents(batch) == "beta one gamma\n");
  CHECK(ToolWriteFile(batch.string(), "same same\n")
            .output.starts_with("wrote "));
  CHECK(ToolEditFile(batch.string(), "same", "other")
            .output.find("matches 2 times") != std::string::npos);
  CHECK(contents(batch) == "same same\n");
  CHECK(ToolEditFile(batch.string(), {{"same", "changed", true},
                                      {"missing", "never written", false}})
            .output.find("edit 2 `old` not found") != std::string::npos);
  CHECK(contents(batch) == "same same\n");

  fs::path registered = root / "registered.txt";
  CHECK(ToolWriteFile(registered.string(), "a a c\n")
            .output.starts_with("wrote "));
  ProcessSupervisor supervisor;
  std::vector<Tool> tools = BuiltinTools(supervisor, root);
  const Tool* read_tool = FindTool(tools, "read_file");
  CHECK(read_tool != nullptr);
  CHECK(read_tool && read_tool->result_chars == ReadFileResultChars());
  CHECK(read_tool &&
        read_tool->parameters["properties"]["limit"]["description"] ==
            "line count (default 1000)");
  fs::path external =
      root.parent_path() / ("uagent-external-list-" + std::to_string(getpid()));
  fs::create_directories(external);
  CHECK(ToolWriteFile((external / "outside.cc").string(), "OUTSIDE_BODY\n")
            .output.starts_with("wrote "));
  const Tool* list_tool = FindTool(tools, "list_dir");
  CHECK(list_tool != nullptr);
  CHECK(list_tool && list_tool->run({{"path", external.string()}}, {})
                             .output.find("OUTSIDE_BODY") == std::string::npos);
  const Tool* edit_tool = FindTool(tools, "edit_file");
  CHECK(edit_tool != nullptr);
  if (edit_tool) {
    CHECK(edit_tool
              ->run({{"path", registered.string()},
                     {"old", "a"},
                     {"new", "b"},
                     {"replace_all", true},
                     {"edits", json::array({{{"old", "c"}, {"new", "d"}}})}},
                    {})
              .output.starts_with("edited "));
    CHECK(contents(registered) == "b b d\n");
    ToolResult invalid = edit_tool->run({{"path", registered.string()},
                                         {"old", "b"},
                                         {"new", "c"},
                                         {"edits", "not-an-array"}},
                                        {});
    CHECK(!invalid.Ok());
    CHECK(invalid.error == ToolErrorCode::kInvalidArguments);
  }

  fs::path private_file = root / "private";
  CHECK(ToolWritePrivateFile(private_file.string(), "secret")
            .output.starts_with("wrote "));
  struct stat st{};
  CHECK(stat(private_file.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);
  CHECK(ToolEditFile(private_file.string(), "secret", "private")
            .output.starts_with("edited "));
  CHECK(stat(private_file.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);
  fs::path ledger = root / "ledger";
  std::string append_error;
  CHECK(AppendPrivateLine(ledger.string(), "one", append_error));
  CHECK(AppendPrivateLine(ledger.string(), "two", append_error));
  CHECK(stat(ledger.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);
  CHECK(ToolReadFile(ledger.string(), 1, -1).output.find("two") !=
        std::string::npos);
  CHECK(PathWithin(CanonicalAccessPath(file.string()),
                   CanonicalAccessPath(root.string())));
  CHECK(!PathWithin(CanonicalAccessPath(root.parent_path().string()),
                    CanonicalAccessPath(root.string())));

  fs::path fifo = root / "pipe";
  CHECK(mkfifo(fifo.c_str(), 0600) == 0);
  ToolResult fifo_read = ToolReadFile(fifo.string(), 1, 1);
  CHECK(!fifo_read.Ok());
  CHECK(fifo_read.error == ToolErrorCode::kPermissionDenied);
  CHECK(fifo_read.output.find("non-regular") != std::string::npos);
  ToolResult fifo_write = ToolWriteFile(fifo.string(), "blocked");
  CHECK(!fifo_write.Ok());
  struct stat fifo_status{};
  CHECK(lstat(fifo.c_str(), &fifo_status) == 0);
  CHECK(S_ISFIFO(fifo_status.st_mode));
  ToolResult directory_read = ToolReadFile(root.string(), 1, 1);
  CHECK(!directory_read.Ok());
  CHECK(directory_read.output.find("non-regular") != std::string::npos);
  fs::path dangling = root / "dangling";
  fs::create_symlink(root / "missing-target", dangling);
  ToolResult dangling_write = ToolWriteFile(dangling.string(), "blocked");
  CHECK(!dangling_write.Ok());
  CHECK(dangling_write.error == ToolErrorCode::kPermissionDenied);
  CHECK(dangling_write.output.find("dangling symlink") != std::string::npos);

  std::error_code ec;
  fs::remove_all(root, ec);
  fs::remove_all(external, ec);
}

void TestTerminalSafety() {
  bool prior = g_tty;
  g_tty = true;
  CHECK(std::string(RST()).find("\033[49m") != std::string::npos);
  CHECK(TerminalSafe("ok\x1b]52;bad\a") == "ok\\x1b]52;bad\\x07");
  CHECK(TerminalSafe("\x1b]0;title\a") == "\\x1b]0;title\\x07");
  CHECK(TerminalSafe("\x1b]8;;https://example.com\a"
                     "label"
                     "\x1b]8;;\a") ==
        "\\x1b]8;;https://example.com\\x07"
        "label"
        "\\x1b]8;;\\x07");
  CHECK(TerminalSafe("\x1b[?1049h\x1b[2J\x1b[H") ==
        "\\x1b[?1049h\\x1b[2J\\x1b[H");
  CHECK(TerminalSafe("replace\rhidden\b!\x7f") ==
        "replace\\rhidden\\x08!\\x7f");
  CHECK(TerminalSafe("line\ncolumn\tvalue") == "line\ncolumn\tvalue");
  std::string long_osc = "\x1b]52;c;" + std::string(4096, 'A') + "\a";
  std::string safe_long_osc = TerminalSafe(long_osc);
  CHECK(safe_long_osc.starts_with("\\x1b]52;c;"));
  CHECK(safe_long_osc.ends_with("\\x07"));
  CHECK(safe_long_osc.find('\x1b') == std::string::npos);
  CHECK(safe_long_osc.find('\a') == std::string::npos);
  g_tty = false;
  CHECK(TerminalSafe("\x1b") == "\x1b");
  g_tty = prior;
  CHECK(DisplayWidth("ASCII") == 5);
  CHECK(DisplayWidth("界") >= 1);
  CHECK(SafeFileComponent("../../escape") == "______escape");
  CHECK(FirstLine(std::string(200, 'x')).size() == 200);
  CHECK(FirstLine("first\nsecond") == "first");
}

}  // namespace uagent
