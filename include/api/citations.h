// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_API_CITATIONS_H_
#define UAGENT_INCLUDE_API_CITATIONS_H_
// Normalize provider citation annotations once for API, trace, and TUI output.

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "include/core/json.h"
#include "include/core/strings.h"

namespace uagent {

struct CitationEntry {
  std::string url;
  std::string title;
  std::string content;
};

inline std::vector<CitationEntry> CitationEntries(const json& annotations) {
  std::vector<CitationEntry> entries;
  if (!annotations.is_array()) return entries;
  for (const json& annotation : annotations) {
    if (!annotation.is_object()) continue;
    const json& citation = annotation.contains("url_citation") &&
                                   annotation["url_citation"].is_object()
                               ? annotation["url_citation"]
                               : annotation;
    std::string url = JsonValue(citation, "url", "");
    bool safe = (url.starts_with("https://") || url.starts_with("http://")) &&
                url.find_first_of(" \t\r\n<>") == std::string::npos;
    if (!safe) continue;
    auto found = std::find_if(
        entries.begin(), entries.end(),
        [&](const CitationEntry& entry) { return entry.url == url; });
    if (found == entries.end()) {
      entries.push_back({std::move(url), JsonValue(citation, "title", ""),
                         JsonValue(citation, "content", "")});
    } else {
      if (found->title.empty()) found->title = JsonValue(citation, "title", "");
      if (found->content.empty()) {
        found->content = JsonValue(citation, "content", "");
      }
    }
    if (entries.size() == 20) break;
  }
  return entries;
}

inline std::string CitationMarkdown(const json& annotations) {
  std::vector<CitationEntry> entries = CitationEntries(annotations);
  if (entries.empty()) return "";
  std::string out = "\n\nSources:\n";
  for (const CitationEntry& entry : entries) {
    out += "- <" + entry.url + ">\n";
  }
  return out;
}

// What one search answer may spend on evidence: enough sources to corroborate
// a claim, enough of each to judge it, and no more context than that is worth.
inline constexpr size_t kEvidenceEntries = 5;
inline constexpr size_t kEvidenceExcerptChars = 800;

inline std::string CitationEvidence(
    const json& annotations, size_t max_entries = kEvidenceEntries,
    size_t excerpt_chars = kEvidenceExcerptChars) {
  std::vector<CitationEntry> entries = CitationEntries(annotations);
  std::string out;
  for (size_t i = 0; i < std::min(entries.size(), max_entries); ++i) {
    const CitationEntry& entry = entries[i];
    out += (out.empty() ? "" : "\n\n") + entry.url;
    if (!entry.title.empty()) out += "\n" + entry.title;
    if (!entry.content.empty()) {
      out += "\n" + Utf8Prefix(entry.content, excerpt_chars);
    }
  }
  return out;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_API_CITATIONS_H_
