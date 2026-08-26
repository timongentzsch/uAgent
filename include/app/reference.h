// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_REFERENCE_H_
#define UAGENT_INCLUDE_APP_REFERENCE_H_
// Release-matched skill references, generated from the same registries the
// binary uses. `uagent --emit-reference DIR` writes them; CI regenerates into a
// temporary directory and fails when the committed copies differ.

#include <string>
#include <vector>

#include "include/core/json.h"

namespace uagent {

struct ReferenceFile {
  std::string name;
  std::string contents;
};

std::vector<ReferenceFile> ReferenceFiles();
json ReferenceManifestJson();
json BuildProvenanceJson();
std::string ReferenceManifest();
bool WriteReferenceFiles(const std::string& directory, std::string& error);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_APP_REFERENCE_H_
