// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_CORE_BUILD_H_
#define UAGENT_INCLUDE_CORE_BUILD_H_
#include <string_view>
namespace uagent {
// Runtime source identity, the same in native targets and remote payloads.
std::string_view BuildIdentity();
}  // namespace uagent
#endif
