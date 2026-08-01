// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_PLATFORM_H_
#define UAGENT_INCLUDE_CORE_PLATFORM_H_

namespace uagent {

// POSIX process environment. Kept behind one boundary so consumers do not
// redeclare the platform symbol.
char** ProcessEnvironment();

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_PLATFORM_H_
