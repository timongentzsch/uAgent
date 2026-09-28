// Copyright 2026 Timon Gentzsch

#include "include/ui/conversation.h"

#include <cstdio>
#include <string>

#include "include/agent/session_view.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/ui/presentation.h"

namespace uagent {

std::string AttachmentDeliveryRows(const json& deliveries) {
  std::string rows;
  if (!deliveries.is_array()) return rows;
  for (const json& delivery : deliveries) {
    if (!delivery.is_object()) continue;
    std::string name = JsonValue(delivery, "name", "");
    std::string kind = JsonValue(delivery, "delivery", "");
    if (name.empty() && kind.empty()) continue;
    if (name.empty()) name = "attachment";
    if (kind.empty()) kind = "attached";
    // Chrome downgrades on non-UTF-8 terminals; the file name is user
    // content and stays byte-preserved.
    rows += std::string(DIM()) + "  " + TerminalSafe(name) +
            AsciiGlyphs(" · ") + TerminalSafe(kind) + RST() + "\n";
  }
  return rows;
}

void PrintConversationHistory(const Conversation& conversation) {
  // The same rows an attached terminal and the browser show.
  const json view = ConversationView(conversation);
  if (JsonValue(view, "more", false)) {
    printf("%s· earlier messages are not shown%s\n", DIM(), RST());
  }
  TerminalPresenter presenter;
  for (const json& block : view["blocks"]) presenter.Block(block);
}

}  // namespace uagent
