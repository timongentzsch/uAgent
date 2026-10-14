// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <string>
#include <utility>

#include "include/browser/browser.h"
#include "include/browser/runtime.h"
#include "include/core/fs.h"
#include "include/core/lease.h"
#include "include/core/platform.h"
#include "include/tools/files.h"
#include "include/transport/session.h"
#include "src/browser/runtime_internal.h"

namespace uagent::browser {
// What needs no page: profiles, who is watching and who drives, and handing
// the browser over and back. Null for anything else.
json Runtime::Control(const std::string& op, const json& command) {
  if (op == "create_profile" || op == "select_profile") {
    if (!profile_error_.empty()) return {{"error", profile_error_}};
    std::string device = JsonValue(command, "device", "");
    if (!session::OpaqueId(device)) return {{"error", "invalid device"}};
    if (!(mode_ == "human" && viewer_ == device) &&
        !(mode_ == "idle" && viewer_.empty() && chrome_pid_ <= 0 &&
          vnc_pid_ <= 0)) {
      return {{"error", "take control before changing Chrome profiles"}};
    }
    if (op == "create_profile") {
      std::string name = ProfileName(JsonValue(command, "name", ""));
      if (name.empty()) return {{"error", "invalid Chrome profile name"}};
      for (const auto& profile : profiles_) {
        if (profile.name == name) {
          return {{"error", "Chrome profile name already exists"}};
        }
      }
      std::string id = RandomToken(16);
      if (id.empty()) return {{"error", "cannot create Chrome profile ID"}};
      profiles_.push_back({id, name});
      if (!SaveProfiles()) {
        profiles_.pop_back();
        return {{"error", "cannot save Chrome profile"}};
      }
      json result = Status();
      result["created_profile_id"] = id;
      return result;
    }
    std::string id = JsonValue(command, "profile_id", "");
    if (std::none_of(
            profiles_.begin(), profiles_.end(),
            [&](const Profile& profile) { return profile.id == id; })) {
      return {{"error", "Chrome profile does not exist"}};
    }
    return SwitchProfile(id);
  }
  if (!profile_error_.empty() && op != "stop") {
    return {{"error", profile_error_}};
  }
  if (op == "agent_status") {
    std::string session = JsonValue(command, "session_id", "");
    if (!session::OpaqueId(session)) return {{"error", "invalid session"}};
    if (mode_ == "human") return {{"error", kHumanControls}};
    if (!agent_session_.empty() && agent_session_ != session) {
      return {{"error", kLeasedElsewhere}};
    }
    return Status();
  }
  if (op == "stop") {
    if (mode_ == "human" && !interaction_.empty()) {
      return {{"error", "finish or cancel the browser interaction first"}};
    }
    if (!viewer_.empty() && viewer_ != JsonValue(command, "device", "")) {
      return {{"error", "another device controls the browser"}};
    }
    Stop();
    SaveHandover();
    return Status();
  }
  if (op == "takeover" || op == "setup_profile") {
    std::string device = JsonValue(command, "device", "");
    if (!session::OpaqueId(device)) return {{"error", "invalid device"}};
    if (!viewer_.empty() && viewer_ != device) {
      return {{"error", "another device controls the browser"}};
    }
    if (op == "setup_profile" && (mode_ != "human" || viewer_ != device)) {
      return {{"error", "take control before signing in to a profile"}};
    }
    const bool setup = profile_setup_ || op == "setup_profile";
    if (setup && !profile_setup_) Stop(true);
    std::string error;
    if (!Start(error, setup)) {
      viewer_ = device;
      mode_ = "human";
      return {{"error", error}};
    }
    viewer_ = device;
    mode_ = "human";
    observation_.clear();
    ++generation_;
    return Status();
  }
  // Any paired device may watch; only the controlling one sends input.
  if (op == "viewer") {
    if (!Alive(chrome_pid_) || !Alive(vnc_pid_)) {
      return {{"error", "the browser is not running"}};
    }
    json result = Status(false);
    result["controller"] =
        mode_ == "human" && viewer_ == JsonValue(command, "device", "");
    return result;
  }
  if (op == "viewer_disconnected") {
    // Only a viewer that saw the current lease hands it back: a stale
    // disconnect after a quick re-takeover carries an older generation.
    if (viewer_ == JsonValue(command, "device", "") &&
        display_ == JsonValue(command, "display", uint64_t{0}) &&
        generation_ == JsonValue(command, "generation", uint64_t{0})) {
      viewer_.clear();
      // Closing the viewer hands control back, unless a login/MFA request
      // or profile sign-in is still waiting on the human's Done.
      if (mode_ == "human" && interaction_.empty() && !profile_setup_) {
        mode_ = agent_session_.empty() ? "idle" : "agent";
        observation_.clear();
        SaveHandover();
      }
      ++generation_;
    }
    return Status();
  }
  if (op == "prepare_done" || op == "done") {
    if (mode_ != "human" || viewer_ != JsonValue(command, "device", "") ||
        interaction_ != JsonValue(command, "interaction_id", "")) {
      return {{"error", "browser interaction changed; refresh"}};
    }
    if (profile_setup_) {
      const std::string controller = viewer_;
      Stop(true);
      std::string error;
      const bool started = Start(error);
      viewer_ = controller;
      mode_ = "human";
      if (!started) {
        profile_setup_ = true;
        return {{"error", error}};
      }
    }
    if (op == "prepare_done") return Status();
  }
  if (op == "done") {
    std::string previous_viewer = viewer_;
    std::string previous_interaction = interaction_;
    std::string previous_mode = mode_;
    viewer_.clear();
    interaction_.clear();
    mode_ = agent_session_.empty() ? "idle" : "agent";
    if (!SaveHandover()) {
      viewer_ = std::move(previous_viewer);
      interaction_ = std::move(previous_interaction);
      mode_ = std::move(previous_mode);
      return {{"error", "cannot persist browser handback"}};
    }
    observation_.clear();
    ++generation_;
    return Status();
  }
  if (op == "release") {
    if (agent_session_ != JsonValue(command, "session_id", "")) {
      return {{"error", "browser lease belongs to another conversation"}};
    }
    if (mode_ == "human") return {{"error", "human controls the browser"}};
    // The turn is over: only its current tab stays, so tabs do not pile up
    // over the profile's life and come back at every start.
    if (Alive(chrome_pid_) && !target_.empty()) {
      for (const json& info : Targets()) {
        const std::string id = JsonValue(info, "targetId", "");
        if (JsonValue(info, "type", "") == "page" && id != target_) {
          Call("Target.closeTarget", {{"targetId", id}});
        }
      }
    }
    agent_session_.clear();
    mode_ = "idle";
    observation_.clear();
    return Status();
  }
  if (op == "cancel_handover") {
    if (agent_session_ != JsonValue(command, "session_id", "") ||
        interaction_ != JsonValue(command, "interaction_id", "") ||
        interaction_.empty()) {
      return {{"error", "browser handover changed"}};
    }
    if (!ClearHandover()) return {{"error", "cannot clear browser handover"}};
    return Status();
  }
  if (op == "request_human" && mode_ == "human" && interaction_.empty()) {
    std::string session = JsonValue(command, "session_id", "");
    std::string interaction = JsonValue(command, "interaction_id", "");
    if (!session::OpaqueId(session) || !session::OpaqueId(interaction) ||
        (!agent_session_.empty() && agent_session_ != session)) {
      return {{"error", "browser handover belongs to another conversation"}};
    }
    std::string previous_session = agent_session_;
    agent_session_ = session;
    interaction_ = interaction;
    observation_.clear();
    if (!SaveHandover()) {
      agent_session_ = std::move(previous_session);
      interaction_.clear();
      return {{"error", "cannot persist browser handover"}};
    }
    return Status();
  }
  return nullptr;
}
}  // namespace uagent::browser
