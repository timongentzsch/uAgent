// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_SESSION_MOVE_H_
#define UAGENT_INCLUDE_APP_SESSION_MOVE_H_
// Moving a conversation and the agents saved beside it to the history of
// another folder. The move is written down first (the journal), so one that
// stopped is finished, never left half done.

#include <map>
#include <string>
#include <vector>

#include "include/core/json.h"
#include "include/core/lease.h"

namespace uagent::session {

struct MovedFile {
  // Session files, in the history of the old folder and of the new one under
  // the same name.
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

// The leases of the sessions still at their old place: while they are held,
// no other process runs one of them, so it can be moved.
using MoveLeases = std::vector<FileLease>;

// Takes them. Nothing has moved yet when this fails; returns why, or empty.
std::string HoldMove(const SessionMove& move, MoveLeases& held);

// Moves the files, rewrites what they hold and points the scheduled runs at
// them. Every step does nothing for what an earlier run did, so it finishes a
// move that stopped. Releases `held` once the files have moved. Returns why
// it could not, or empty.
std::string CarryOutMove(const SessionMove& move, MoveLeases& held);

}  // namespace uagent::session

#endif  // UAGENT_INCLUDE_APP_SESSION_MOVE_H_
