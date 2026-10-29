// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_SRC_APP_SESSION_TERMINAL_INTERNAL_H_
#define UAGENT_SRC_APP_SESSION_TERMINAL_INTERNAL_H_
// One terminal attached to a session runtime. TerminalMain and the input loop
// live in session_terminal.cc, the frames to and from the runtime in
// session_terminal_link.cc, the client's own slash commands in
// session_terminal_commands.cc and the live region in
// session_terminal_screen.cc.

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/app/session.h"
#include "include/cli/cli.h"
#include "include/core/detail.h"
#include "include/core/json.h"
#include "include/core/runtime_config.h"
#include "include/ui/input_decoder.h"
#include "include/ui/interactive.h"
#include "include/ui/live_region.h"
#include "include/ui/presentation.h"

namespace uagent::session {

// The level this terminal shows: its --detail when given, else the
// configured one.
const DetailLevel& ConfiguredDetail(const RuntimeConfig::Values& overrides);

class Terminal {
 public:
  // `draft` starts the composer, e.g. the message a rewind forked before.
  // `pinned`: --detail set this terminal's level, so it follows no other.
  // `keys`: what was typed and not yet read, kept from one attach to the next.
  Terminal(Connection connection, std::string path, const DetailLevel& detail,
           bool pinned, TerminalInputDecoder& keys, std::string draft = "")
      : connection_(std::move(connection)),
        path_(std::move(path)),
        draft_(std::move(draft)),
        region_(output_),
        composer_(output_, region_, keys),
        detail_(&detail),
        pinned_(pinned) {
    presenter_.SetDetail(detail);
  }
  const DetailLevel& Detail() const { return *detail_; }

  int Run(const std::vector<std::string>& attachments);
  const std::string& Next() const { return next_; }
  // What the next session's composer starts with.
  const std::string& Carry() const { return carry_; }
  const std::string& SideQuestion() const { return side_question_; }
  void Ask(const std::string& text) {
    Send({{"kind", "submit"}, {"text", text}});
  }

 private:
  // What a line did: nothing here (it goes on as the reply or to the
  // runtime), all of it, or ended this session's input loop.
  enum class Command { kPass, kDone, kLeave };

  // The input loop (session_terminal.cc).
  // Starts the reader and, on a terminal, raw input; false when it cannot.
  bool Attach();
  // Waits for a key, a frame, a signal or the next tick; `keyed` says
  // whether stdin is ready. False when poll failed.
  bool Wait(bool running,
            std::optional<std::chrono::steady_clock::time_point> frame,
            bool& keyed);
  // Ctrl+C with nothing to stop: the first says how to detach, the second
  // asks to (true). A Ctrl+C that aborts goes to the runtime.
  bool Interrupted();
  // What was typed; empty when stdin closed.
  std::optional<InteractiveInputEvent> ReadInput(std::string& cooked);
  // Acts on one input: a key, a reply to `decision` or a message.
  Command Handle(const InteractiveInputEvent& input, const json& pending,
                 const std::string& decision, bool running,
                 std::vector<std::string>& files);
  // Ends the attach: says what staged files were dropped, lets the reader
  // finish and gives the terminal back.
  int Detach(const std::vector<std::string>& files, int exit_code);

  // The client's own slash commands (session_terminal_commands.cc).
  // The slash commands this client performs itself. Leaving the session
  // works while a decision is pending; the rest is then the reply's text.
  Command HandleCommand(const ParsedSlashCommand& slash, bool deciding,
                        std::vector<std::string>& files);
  // Those that leave or redraw the session; empty for the rest.
  std::optional<Command> SessionCommand(const ParsedSlashCommand& slash);
  Command ConversationCommand(const ParsedSlashCommand& slash,
                              std::vector<std::string>& files);
  Command SetDetail(const std::string& argument);
  // The folder this session works for: an agent's first ancestor's, else
  // the session's own.
  std::string Folder() const;
  // A session id prefix or a unique title match among the folder's sessions.
  std::string MatchFolderSession(const std::string& argument) const;
  // Says so when a turn is running, which switching would abandon.
  bool TurnActive(const std::string& before);
  // Ends the input loop to open `target` next, unless a turn is running.
  Command Leave(const std::string& target, const std::string& before);
  // Shows the conversation again at another level: the reader draws the
  // tree it holds anew once the runtime answers a refresh.
  void Restyle(const DetailLevel& detail);

  // Frames to and from the runtime (session_terminal_link.cc).
  // Answers the decision `id`; `extra` adds fields such as "cancelled".
  void Reply(const std::string& id, json text,
             const json& extra = json::object());
  void Send(json command);
  void Receive(const json& frame);
  void ReceiveState(const json& frame);
  // False when the frame showed nothing, so nothing is flushed or woken.
  bool ReceiveEvent(const json& frame);
  bool ReceiveOutcome(const json& frame);

  // The live region (session_terminal_screen.cc).
  // Shows a decision that just arrived, or gives the composer back once it
  // is gone. True when it was settled here and the loop starts over.
  bool PresentDecision(const json& pending, const std::string& decision);
  // Takes the output written so far into the live region, unpainted.
  void Absorb();
  // Brings the live region up to date: new output, the status row and the
  // draft, repainted only where they changed.
  void Paint();

  Connection connection_;
  std::string path_, decision_, draft_, tail_, waiting_, next_;
  InteractiveOutput output_;
  LiveRegion region_;
  RawComposer composer_;
  TerminalPresenter presenter_;
  Pipe stop_, wake_;
  std::thread reader_;
  std::mutex mutex_, send_mutex_;
  json state_ = json::object(), pending_;
  bool model_asked_ = false;
  bool running_ = false, history_ = false, raw_ = false;
  bool quit_hint_ = false;
  std::atomic<bool> interrupting_{false};
  std::atomic<bool> disconnected_{false}, detaching_{false}, ended_{false},
      failed_{false};
  std::atomic<bool> navigate_{false};
  std::atomic<bool> rewinding_{false};
  std::string carry_, side_question_;
  std::atomic<const DetailLevel*> detail_;
  const bool pinned_;
  std::atomic<bool> input_blocked_{true}, interaction_{false};
  std::set<std::string> own_requests_;
  // What every request this terminal sends starts with, so a message it
  // echoed as it was typed is known when it comes back.
  const std::string requests_ = RandomToken(8);
  // The transcript's tree as history and events build it, so a live node is
  // drawn from the same tree a resumed one is.
  json transcript_ = json::object();
};

}  // namespace uagent::session

#endif  // UAGENT_SRC_APP_SESSION_TERMINAL_INTERNAL_H_
