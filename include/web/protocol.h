// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_WEB_PROTOCOL_H_
#define UAGENT_INCLUDE_WEB_PROTOCOL_H_
#include <string>

#include "include/app/session.h"
namespace uagent::web {
using session::kCommandBytes;
using session::kFrameBytes;
using session::kGlobalAssetBytes;
using session::kProtocol;
using session::kQueueBytes;
using session::kSessionAssetBytes;
using session::kUploadBytes;
using session::kUploadCount;
using session::OpaqueId;
using session::Pipe;
using session::RandomToken;
using session::ReadFrames;
using session::WriteFrame;
inline constexpr size_t kScheduledConcurrency = 4;
struct WebOptions {
  int port = 8080;
  std::string origin;
  std::string push_contact;
};
int MasterMain(const WebOptions& options, const char* executable);
}  // namespace uagent::web
#endif
