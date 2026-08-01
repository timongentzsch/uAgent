// Copyright 2026 Timon Gentzsch

#include "include/core/platform.h"

extern char** environ;

namespace uagent {

char** ProcessEnvironment() { return environ; }

}  // namespace uagent
