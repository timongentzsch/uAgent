// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_PRESENTATION_H_
#define UAGENT_INCLUDE_UI_PRESENTATION_H_
// The terminal's transcript: the host's tree of nodes drawn open to the
// level's depth (tree_render.cc), and what streams live (presentation.cc).

#include <functional>
#include <memory>
#include <set>
#include <string>

#include "include/core/detail.h"
#include "include/core/events.h"

namespace uagent {

std::string ColorizeDiffLines(std::string_view text);

// One dim "name · delivery" row per recorded attachment delivery.
std::string AttachmentDeliveryRows(const json& deliveries);

// The speaker's row above a reply, live and from history.
void PrintMessageHeader();

class TerminalSpinner;

class TerminalPresenter {
 public:
  TerminalPresenter();
  ~TerminalPresenter();
  TerminalPresenter(const TerminalPresenter&) = delete;
  TerminalPresenter& operator=(const TerminalPresenter&) = delete;

  // What streams live: replies, thinking, the spinner and notices.
  void Consume(const Event& event) noexcept;
  void Consume(const AppEvent& event) noexcept;
  // The tree a view holds ({nodes}), all of it, as it stands.
  void Draw(const json& view);
  // Draws what settled in `view` since it was drawn: a node is drawn once it
  // has finished, so a step's calls follow its reply.
  void Update(const json& view, const json& changed);
  // `id` is on screen already, e.g. a message this terminal echoed.
  void Drawn(const std::string& id) { drawn_.insert(id); }
  void Finish() noexcept;
  void SetDetail(const DetailLevel& detail) { detail_ = &detail; }
  const DetailLevel& Detail() const { return *detail_; }

 private:
  struct State;
  void DrawTree(const json& view, bool live);
  const DetailLevel* detail_ = &DetailFor("default");
  // The settled nodes on screen, by id.
  std::set<std::string, std::less<>> drawn_;
  // A streamed reply left its last line open.
  bool line_open_ = false;
  // The turn's speaker row is on screen: its later replies stream under it.
  bool spoke_ = false;
  std::unique_ptr<State> state_;
  std::unique_ptr<TerminalSpinner> spinner_;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_PRESENTATION_H_
