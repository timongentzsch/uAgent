// Copyright 2026 Timon Gentzsch

// RuntimeConfig coherence: every registry descriptor that names a struct
// field must surface in DiagnosticJson/ProvenanceJson (i.e. have a table
// entry in env.cc), and every DiagnosticJson key must trace back to a
// descriptor field or an explicitly allowlisted derived key. A struct
// member added without a table entry fails here instead of silently
// keeping its hard-coded default.

#include <set>
#include <string>

#include "include/core/config_registry.h"
#include "include/core/env.h"
#include "include/core/runtime_config.h"
#include "tests/unit/test_support.h"

namespace uagent {

// Derived keys DiagnosticJson adds on top of the option tables.
bool IsDerivedDiagnosticKey(const std::string& key) {
  return key == "auto_compact_pct" || key == "auto_compact_tokens";
}

void TestRuntimeConfigCoherence() {
  const RuntimeConfig config = RuntimeConfig::FromEnvironment();
  const json diagnostic = config.DiagnosticJson();
  REQUIRE(diagnostic.is_object());

  std::set<std::string> registry_fields;
  for (const ConfigDescriptor& descriptor : ConfigRegistry()) {
    if (descriptor.field.empty()) continue;
    registry_fields.insert(std::string(descriptor.field));
  }
  CHECK(!registry_fields.empty());

  for (const std::string& field : registry_fields) {
    CHECK(diagnostic.contains(field));
  }

  // Every diagnostic key traces back to the registry or the derived list.
  for (auto it = diagnostic.begin(); it != diagnostic.end(); ++it) {
    CHECK(registry_fields.count(it.key()) > 0 ||
          IsDerivedDiagnosticKey(it.key()));
  }

  // FromValues round-trips a renamed field through the same tables.
  RuntimeConfig::Values values;
  values["UAGENT_APPROVAL"] = "yolo";
  const RuntimeConfig reloaded = RuntimeConfig::FromValues(values);
  CHECK(reloaded.approval == "yolo");
  CHECK(reloaded.DiagnosticJson().contains("approval"));
  CHECK(RuntimeConfigField("UAGENT_APPROVAL") == "approval");
}

}  // namespace uagent
