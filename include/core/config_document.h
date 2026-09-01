// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_CONFIG_DOCUMENT_H_
#define UAGENT_INCLUDE_CORE_CONFIG_DOCUMENT_H_
// A µAgent config file kept as the exact lines it was read as, so a targeted
// edit can change one assignment and leave comments, blank lines, ordering,
// unknown keys and line endings byte-identical.

#include <string>
#include <string_view>
#include <vector>

namespace uagent {

struct ConfigAssignment {
  std::string key;
  std::string value;
  bool exported = false;
};

// The assignment grammar shared by the loader, byte-preserving editor, and
// approval preview. Values remain quoted; callers choose when to unquote.
bool ParseConfigAssignment(const std::string& line,
                           ConfigAssignment& assignment);

class ConfigDocument {
 public:
  static ConfigDocument Parse(const std::string& bytes);

  // Assignments for `key`. More than one is ambiguous: the loader takes the
  // last, but a user editing by hand may have meant either, so a targeted
  // change refuses rather than guessing.
  size_t Count(std::string_view key) const;

  // Rewrites the existing assignment in place, keeping its `export ` prefix and
  // position, or appends one. False when the key is assigned more than once or
  // the value cannot be represented in this grammar.
  bool Set(const std::string& key, const std::string& value,
           std::string& error);
  bool Unset(const std::string& key, std::string& error);

  std::string Render() const;

 private:
  std::vector<std::string> lines_;  // no terminators
  std::string terminator_ = "\n";
  bool final_newline_ = true;
};

// The literal form of `value` in a config file, or empty with `error` set when
// the grammar cannot represent it without an escape it does not understand.
bool ConfigValueLiteral(const std::string& value, std::string& literal,
                        std::string& error);

// A unified diff of two config files, for display only.
std::string ConfigUnifiedDiff(const std::string& before,
                              const std::string& after,
                              const std::string& label);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_CONFIG_DOCUMENT_H_
