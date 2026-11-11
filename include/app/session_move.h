// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_SESSION_MOVE_H_
#define UAGENT_INCLUDE_APP_SESSION_MOVE_H_
// Moving a conversation and its agents to another folder's history. The move
// is journaled first, so one that stopped is finished, never left half done.

#include <map>
#include <string>
#include <vector>

#include "include/core/json.h"
#include "include/core/lease.h"

namespace uagent::session {

struct MovedFile {
  // The session file in the old folder's history and in the new one's.
  std::string from, to;
};

struct SessionMove {
  std::string from_cwd, to_cwd;
  // What the conversation is told, and shows as a row.
  std::string note;
  // The conversation first, then its agents.
  std::vector<MovedFile> files;
};

json MoveJson(const SessionMove& move);
// Whether every path lies where a move puts it.
bool ValidMove(const SessionMove& move);
// False unless `value` is a valid move.
bool MoveFromJson(const json& value, SessionMove& move);

// Each moved session's old id, with its new one.
std::map<std::string, std::string> MovedIds(const SessionMove& move);

// Leases of the sessions at their old place: while held, no process runs one.
using MoveLeases = std::vector<FileLease>;

// Takes them. Nothing has moved yet when this fails; returns why, or empty.
std::string HoldMove(const SessionMove& move, MoveLeases& held);

// Moves the files, rewrites them and repoints the scheduled runs; each step
// skips what an earlier run did. Releases `held` once the files have moved.
std::string CarryOutMove(const SessionMove& move, MoveLeases& held);

}  // namespace uagent::session

#endif  // UAGENT_INCLUDE_APP_SESSION_MOVE_H_
