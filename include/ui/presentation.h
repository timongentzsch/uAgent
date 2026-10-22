// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_PRESENTATION_H_
#define UAGENT_INCLUDE_UI_PRESENTATION_H_
// Terminal rendering of the transcript view's rows and of live events.

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>

#include "include/core/events.h"
#include "include/core/verbosity.h"

namespace uagent {

// Unified-diff line styling shared by change receipts and approval previews.
std::string ColorizeDiffLines(std::string_view text);

// One dim "name · delivery" row per recorded attachment delivery, as the web
// shows a message's files. Empty when there is nothing to show.
std::string AttachmentDeliveryRows(const json& deliveries);

class TerminalSpinner;

class TerminalPresenter {
 public:
  TerminalPresenter();
  ~TerminalPresenter();
  TerminalPresenter(const TerminalPresenter&) = delete;
  TerminalPresenter& operator=(const TerminalPresenter&) = delete;

  void Consume(const Event& event) noexcept;
  void Consume(const AppEvent& event) noexcept;
  // One row of the transcript view, live or from history. A tool row draws
  // its call line when it first arrives and its result once it has finished,
  // so a live row that arrives twice reads as the same row from history.
  void Block(const json& block);
  void Finish() noexcept;
  // What the UAGENT_VERBOSITY level shows; see include/core/verbosity.h.
  void SetDetail(const DetailPolicy& detail) { detail_ = &detail; }
  const DetailPolicy& Detail() const { return *detail_; }

 private:
  struct State;
  // A tool row through the level: its call line, then its result.
  void ToolRow(const json& block);
  // The row for the work counted since the last one, or nothing.
  std::string WorkLine();
  const DetailPolicy* detail_ = &DetailFor("default");
  int steps_ = 0;
  // The tool rows drawn since the last work row, by id, and whether each
  // has drawn its result; and the files the turn's edits changed.
  std::map<std::string, bool> rows_;
  std::set<std::string> edited_;
  std::unique_ptr<State> state_;
  std::unique_ptr<TerminalSpinner> spinner_;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_PRESENTATION_H_
