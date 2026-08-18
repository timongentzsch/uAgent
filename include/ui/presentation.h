// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_UI_PRESENTATION_H_
#define UAGENT_INCLUDE_UI_PRESENTATION_H_
// Terminal-only rendering of provider-independent observation records.

#include <memory>
#include <string>

#include "include/core/events.h"

namespace uagent {

// Generic plain-text activity normalization: collapse whitespace and drop
// common lightweight formatting punctuation. It never branches on provider,
// model, or inferred reasoning syntax.
std::string StripDisplayMarkdown(const std::string& text);

class TerminalPresenter {
 public:
  TerminalPresenter();
  ~TerminalPresenter();
  TerminalPresenter(const TerminalPresenter&) = delete;
  TerminalPresenter& operator=(const TerminalPresenter&) = delete;

  void Consume(const Event& event) noexcept;
  void Finish() noexcept;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

void PrintPresentation(const PresentationRecord& record) noexcept;

}  // namespace uagent

#endif  // UAGENT_INCLUDE_UI_PRESENTATION_H_
