// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MCP_ELICIT_H_
#define UAGENT_INCLUDE_MCP_ELICIT_H_
// MCP elicitation: the flat form a server may ask the person to fill in, the
// URL it may ask them to open, and the ElicitResult sent back on the retry.

#include <cstddef>
#include <optional>
#include <string>

#include "include/core/json.h"
#include "include/mcp/server.h"

namespace uagent {

// A field's choices as [{value, label}]; empty for a field typed freely.
json ElicitChoices(const json& field);

// Why `schema` is not a requestedSchema the protocol allows.
std::optional<std::string> ElicitSchemaIssue(const json& schema);

// Why `content` does not answer `schema`.
std::optional<std::string> ElicitContentIssue(const json& schema,
                                              const json& content);

// A typed line as the field's value: empty keeps its default (null without
// one); a choice is its number, value or label, several comma-separated.
bool ElicitParseField(const json& field, const std::string& text, json& value,
                      std::string& error);

// Why `reply` is not an ElicitResult for the request the person was shown.
std::optional<std::string> ElicitReplyIssue(const json& shown,
                                            const json& reply);

// One elicitation/create put to the person; the ElicitResult. A request that
// cannot be shown is declined, and a line in `note` says why.
json McpElicit(const McpServer& server, const json& params, std::string& note);

inline constexpr size_t kMcpElicitationsPerRound = 4;

// The inputResponses to one round's inputRequests; false at a request this
// client does not answer. The first elicitation not accepted cancels the rest
// unseen. `wait` is set when a link was accepted: the retry then waits on the
// person's browser, not on mcp.timeout.
bool McpInputResponses(const McpServer& server, const json& requests,
                       json& responses, bool& wait, std::string& note);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MCP_ELICIT_H_
