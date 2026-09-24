// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_PRESENTATION_H_
#define UAGENT_INCLUDE_UI_PRESENTATION_H_
// Terminal-only rendering of provider-independent observation records.

#include <cstdint>
#include <memory>
#include <string>

#include "include/core/events.h"

namespace uagent {

bool PrintSearchReceipt(int64_t searches, const json& annotations,
                        bool details = false, bool line_open = false);

void PrintCitationSources(const json& annotations);

// Unified-diff line styling shared by change receipts and approval previews.
const char* DiffLineStyle(std::string_view line);
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
  // /verbose: full reasoning, one row per call, whole tool output, every
  // source and routine notices. A presentation choice, never a runtime one.
  void SetDetailed(bool detailed) { detailed_ = detailed; }
  bool Detailed() const { return detailed_; }

 private:
  struct State;
  bool detailed_ = false;
  std::unique_ptr<State> state_;
  std::unique_ptr<TerminalSpinner> spinner_;
};

void PrintMessageHeader();
std::string TurnStatsLine(const json& summary);

void PrintPresentation(const PresentationRecord& record,
                       bool detailed = false) noexcept;

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_PRESENTATION_H_
