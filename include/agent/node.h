// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_NODE_H_
#define UAGENT_INCLUDE_AGENT_NODE_H_
// The transcript as one tree of nodes, the only shape a client draws:
//   {id, parent, kind, headline {verb, target}, result {word, facts...},
//    state, routine, time?, body {kind, text, ...}}
// kind: turn, message, work, group, call, thinking, notice, footer;
// state: running, ok, failed, cancelled. `parent` is null at the root.

#include <string>
#include <string_view>
#include <vector>

#include "include/core/json.h"

namespace uagent {

json MakeNode(std::string id, std::string_view parent, std::string_view kind,
              json fields = json::object());
// A body of `kind` holding the start of `text`; none for blank text.
void SetBody(json& node, std::string_view kind, const std::string& text,
             bool truncated = false);
bool Blank(std::string_view text);
// The work replying to `message`; "" for none.
std::string WorkId(std::string_view message);
std::string ThinkingId(std::string_view message);
// A reply's thinking: a node of its own under its work.
json ThinkingNode(std::string_view message, std::string_view work,
                  json fields = json::object());

// Present verb while it runs, past after; a refused call reads as `sent`.
json CallHeadline(const json& view, std::string_view state,
                  const std::string& sent);
// Sits under its group, else under `work`.
json CallNode(const json& record, std::string_view work);
// The files a call handed to the person, under its turn; null for none.
json FilesNode(const json& record, std::string_view turn);
// Finished once `facts` ({items, edited, errors}) hold outcome and duration_ms.
json WorkNode(const std::string& message, const json& facts);
// A receipt or a compaction becomes a notice, a turn's summary its footer.
json EntryNode(const json& entry);

}  // namespace uagent
#endif  // UAGENT_INCLUDE_AGENT_NODE_H_
