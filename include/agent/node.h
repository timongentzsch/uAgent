// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_NODE_H_
#define UAGENT_INCLUDE_AGENT_NODE_H_
// The transcript as one tree of nodes, the only shape a client draws:
//   {id, parent, kind, headline {verb, target}, result {word, facts...},
//    state, routine, time?, body {kind, text, ...}}
// `parent` is null at the root; siblings keep the order they arrived in.
// Kinds: turn (a message of yours), message, work (the steps replying to a
// message of yours, under it), group (like calls of one step), call,
// thinking, notice and footer. State is running, ok, failed or cancelled.
// Blank text is never a body.

#include <string>
#include <string_view>
#include <vector>

#include "include/core/json.h"

namespace uagent {

json MakeNode(std::string id, std::string_view parent, std::string_view kind,
              json fields = json::object());
// Gives `node` a body of `kind` holding the start of `text`, unless `text`
// is blank.
void SetBody(json& node, std::string_view kind, const std::string& text,
             bool truncated = false);
bool Blank(std::string_view text);
// The work replying to `message`; "" for none.
std::string WorkId(std::string_view message);
std::string ThinkingId(std::string_view message);
// A reply's thinking: a node of its own under its work.
json ThinkingNode(std::string_view message, std::string_view work,
                  json fields = json::object());

// A call's headline: its present verb while it runs, its past one after. A
// call refused before it ran has no verb and reads as what it sent.
json CallHeadline(const json& view, std::string_view state,
                  const std::string& sent);
// A call as recorded or announced (tool.call); it sits under its group, else
// under `work`.
json CallNode(const json& record, std::string_view work);
// The files a call handed to the person, as a node of their own under its
// turn, so they show at every depth; null when it handed none. The call's
// node keeps its other parts.
json FilesNode(const json& record, std::string_view turn);
// The work replying to `message` from its facts ({items, edited, errors}),
// finished once they hold its `outcome` and `duration_ms`: headline "Worked"
// and its counts ("6 steps · 1 error"), and how long it took as the result
// word.
json WorkNode(const std::string& message, const json& facts);
// The groups a step's calls fold into, from their activities: adjacent,
// successful calls of one groupable intent. Each {id, verb, members}.
std::vector<json> GroupCalls(const std::vector<json>& activities);
// A group as recorded: {verb, calls, work}: "Explored" and "3 calls".
json GroupNode(const std::string& id, const json& group);
// A recorded entry: a receipt or a compaction is a notice, a turn's summary
// its footer.
json EntryNode(const json& entry);

}  // namespace uagent
#endif  // UAGENT_INCLUDE_AGENT_NODE_H_
