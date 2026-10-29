// Copyright 2026 Timon Gentzsch

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "include/web/terminal.h"
#include "tests/unit/test_support.h"

namespace uagent {
namespace {

// Reads `viewer` until its output holds `marker` or it ends; all it read.
std::string ReadUntil(web::TerminalViewer& viewer, const std::string& marker,
                      web::TerminalOutput* last = nullptr) {
  std::string seen;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(BudgetMs(10000));
  while (std::chrono::steady_clock::now() < deadline) {
    web::TerminalOutput out = viewer.Read(std::chrono::milliseconds(100));
    seen += out.bytes;
    if (last) *last = out;
    if (seen.find(marker) != std::string::npos || out.ended || out.replaced) {
      break;
    }
  }
  return seen;
}

}  // namespace

void TestWebTerminal() {
  TestWorkspace workspace("web-terminal");
  // A plain shell, so the user's own profile cannot change what it prints.
  ScopedEnv shell("SHELL", "/bin/sh");
  ScopedEnv secret("OPENROUTER_API_KEY", "planted-openrouter");
  ScopedEnv setting("UAGENT_API_KEY", "planted-uagent");
  ScopedEnv agent("SSH_AUTH_SOCK", "/tmp/agent.sock");
  web::Terminals terminals(2, std::chrono::minutes(1));
  std::string error;
  const std::string folder = workspace.workspace.string();
  CHECK(terminals.Open("relative", folder, error).empty() && !error.empty());
  error.clear();
  WriteFile(workspace.workspace / "file", "x");
  CHECK(terminals.Open((workspace.workspace / "file").string(), folder, error)
            .empty());
  CHECK(terminals.Attach("unknown") == nullptr);
  error.clear();

  const std::string id = terminals.Open(folder, folder, error);
  CHECK(!id.empty() && error.empty());
  CHECK(terminals.List().size() == 1 && terminals.List()[0].id == id &&
        terminals.List()[0].folder == folder);
  const std::string cwd = terminals.List()[0].cwd;
  auto viewer = terminals.Attach(id);
  CHECK(viewer != nullptr);
  CHECK(viewer->Resize(30, 100));
  CHECK(viewer->Write(
      "pwd; stty size; echo \"[${OPENROUTER_API_KEY:-none}]\" "
      "\"[${UAGENT_API_KEY:-none}]\" \"[${UAGENT_APPROVAL:-none}]\" "
      "\"[$SSH_AUTH_SOCK]\" \"[$TERM]\"\n"));
  std::string seen = ReadUntil(*viewer, "[xterm-256color]");
  CHECK(seen.find(cwd + "\r\n") != std::string::npos);
  CHECK(seen.find("30 100") != std::string::npos);
  // µAgent's keys and approval mode stay out; the person's ssh agent stays in.
  CHECK(seen.find("[none] [none] [none] [/tmp/agent.sock] [xterm-256color]") !=
        std::string::npos);
  CHECK(seen.find("planted") == std::string::npos);

  // A second device takes the shell over and is shown what it printed; the
  // first learns it was replaced and can no longer type.
  auto second = terminals.Attach(id);
  web::TerminalOutput last;
  ReadUntil(*viewer, "never printed", &last);
  CHECK(last.replaced);
  CHECK(!viewer->Write("echo stale\n"));
  CHECK(ReadUntil(*second, "[xterm-256color]").find("30 100") !=
        std::string::npos);
  viewer.reset();

  // At most two shells; closing one ends it for its viewer.
  const std::string other = terminals.Open("", folder, error);
  CHECK(!other.empty());
  CHECK(terminals.Open("", folder, error).empty() &&
        error.find("at most 2") != std::string::npos);
  CHECK(terminals.Close(id));
  CHECK(!terminals.Close(id));
  ReadUntil(*second, "never printed", &last);
  CHECK(last.ended);

  // A shell that exits ends with its code.
  auto exiting = terminals.Attach(other);
  CHECK(exiting->Write("exit 3\n"));
  ReadUntil(*exiting, "never printed", &last);
  CHECK(last.ended && last.code == 3);
  CHECK(WaitUntil([&] { return terminals.List().empty(); }, BudgetMs(5000)));

  // With no device attached and nothing printed for the idle time, a shell
  // ends; one a device holds stays until it lets go.
  web::Terminals idle(2, std::chrono::milliseconds(200));
  const std::string unattended = idle.Open("", folder, error);
  const std::string held = idle.Open("", folder, error);
  auto holder = idle.Attach(held);
  CHECK(WaitUntil(
      [&] { return idle.List().size() == 1 && idle.List()[0].id == held; },
      BudgetMs(5000)));
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  CHECK(idle.List().size() == 1);
  holder.reset();
  CHECK(WaitUntil([&] { return idle.List().empty(); }, BudgetMs(5000)));
  CHECK(!unattended.empty());

  // The page's own shell, of no folder, is never listed, has one viewer
  // ever and ends when it leaves; the list's listener hears each change.
  std::atomic<int> changes = 0;
  web::Terminals page(2, std::chrono::minutes(1), [&] { ++changes; });
  const std::string own = page.Open("", "", error);
  CHECK(!own.empty() && page.List().empty() && changes == 1);
  auto only = page.Attach(own);
  CHECK(only != nullptr && page.Attach(own) == nullptr);
  only.reset();
  CHECK(WaitUntil([&] { return changes == 2; }, BudgetMs(5000)));
  CHECK(!page.Close(own));
}

}  // namespace uagent
