// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_PRESENTATION_H_
#define UAGENT_INCLUDE_UI_PRESENTATION_H_
// Terminal-only rendering of provider-independent observation records.

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

class TerminalSpinner;

class TerminalPresenter {
 public:
  TerminalPresenter();
  ~TerminalPresenter();
  TerminalPresenter(const TerminalPresenter&) = delete;
  TerminalPresenter& operator=(const TerminalPresenter&) = delete;

  void Consume(const Event& event) noexcept;
  void Consume(const AppEvent& event) noexcept;
  void Block(const json& block);
  void Finish() noexcept;
  // What the UAGENT_VERBOSITY level shows; see include/core/verbosity.h.
  void SetDetail(const DetailPolicy& detail) { detail_ = &detail; }
  const DetailPolicy& Detail() const { return *detail_; }

 private:
  struct State;
  // A record through the level: where a turn's work is one row, what
  // succeeded is counted instead of printed and only a failure keeps its row.
  void Present(const PresentationRecord& record);
  // The row for the work counted since the last one, or nothing.
  std::string WorkLine();
  const DetailPolicy* detail_ = &DetailFor("default");
  int steps_ = 0;
  // Each call's target by its id, and the files the turn's edits changed.
  std::map<std::string, std::string> targets_;
  std::set<std::string> edited_;
  std::unique_ptr<State> state_;
  std::unique_ptr<TerminalSpinner> spinner_;
};

// One record as the policy lays it out. The fold of a whole turn into one
// row is the presenter's: it needs the turn.
void PrintPresentation(
    const PresentationRecord& record,
    const DetailPolicy& detail = DetailFor("default")) noexcept;

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_PRESENTATION_H_
