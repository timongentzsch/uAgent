// Copyright 2026 Timon Gentzsch

#include "include/tools/ask.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include "include/agent/path_policy.h"
#include "include/core/fs.h"
#include "include/core/strings.h"
#include "include/media/attachments.h"

namespace uagent {
namespace {
constexpr size_t kPreviewLines = 40;
constexpr size_t kPreviewBytes = 4000;

bool ImageFile(const std::string& path) {
  std::string extension = std::filesystem::path(path).extension().string();
  std::ranges::transform(extension, extension.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  for (const char* known : {".png", ".jpg", ".jpeg", ".gif", ".webp", ".svg"}) {
    if (extension == known) return true;
  }
  return false;
}

// What an option's image may be: an image the agent made in the workspace,
// never a file outside it, µAgent's own configuration or the browser profile.
std::optional<std::string> OptionImageIssue(const std::string& path) {
  if (!ImageFile(path)) {
    return "an option's image must be a .png, .jpg, .gif, .webp or .svg file";
  }
  if (PathApprovalRequired(path, CanonicalCwd()) || HiddenPath(path) ||
      PathApprovalClass(path, PathAccess::kRead) != ApprovalClass::kNone) {
    return "an option's image must be a file you made in the workspace";
  }
  return std::nullopt;
}

// The questions as clients see them: each option image a stored asset
// (with its path, for a terminal), or dropped with a note when it cannot be
// read. Previews and alt text travel as the model wrote them.
json ShownQuestions(json questions, const AskImage& image,
                    std::string& notes) {
  for (json& question : questions) {
    for (json& option : question["options"]) {
      const std::string path = JsonValue(option, "image", "");
      if (path.empty()) continue;
      // Checked again as the call runs: an earlier call in the batch can
      // have made the path a link to somewhere else.
      if (auto issue = OptionImageIssue(path)) {
        notes += "\n(" + JsonValue(option, "label", "") +
                 ": its image was not shown: " + *issue + ")";
        option.erase("image");
        continue;
      }
      json shown = {{"path", path}};
      if (image) {
        std::string error;
        json asset = image(path, error);
        if (!error.empty()) {
          notes += "\n(" + JsonValue(option, "label", "") +
                   ": its image was not shown: " + error + ")";
          option.erase("image");
          continue;
        }
        shown["id"] = JsonValue(asset, "id", "");
        shown["name"] = JsonValue(asset, "name", "");
      }
      option["image"] = std::move(shown);
    }
  }
  return questions;
}
}  // namespace

std::optional<std::string> AskQuestionsIssue(const json& questions) {
  if (!questions.is_array() || questions.empty() || questions.size() > 4) {
    return "ask 1 to 4 questions";
  }
  for (const json& question : questions) {
    const json* options = JsonArray(question, "options");
    const std::string header = JsonValue(question, "header", "");
    const auto characters = std::ranges::count_if(
        header, [](unsigned char byte) { return (byte & 0xC0) != 0x80; });
    if (JsonValue(question, "question", "").empty() || header.empty() ||
        characters > 12) {
      return "each question needs its text and a header of at most 12 "
             "characters";
    }
    if (!options || options->size() < 2 || options->size() > 4) {
      return "each question needs 2 to 4 options; Other is always offered";
    }
    for (const json& option : *options) {
      if (JsonValue(option, "label", "").empty()) {
        return "each option needs a label";
      }
      const std::string image = JsonValue(option, "image", "");
      if (!image.empty()) {
        if (auto issue = OptionImageIssue(image)) return issue;
        if (JsonValue(option, "description", "").empty()) {
          return "an option with an image needs a description, its alt text";
        }
      }
      const std::string preview = JsonValue(option, "preview", "");
      if (preview.size() > kPreviewBytes ||
          static_cast<size_t>(std::ranges::count(preview, '\n')) >=
              kPreviewLines) {
        return "a preview holds at most 40 lines and 4000 bytes";
      }
    }
  }
  return std::nullopt;
}

Tool AskTool(AskPerson ask, AskImage image) {
  Tool tool = MakeTool(
      "ask",
      "Ask the user 1 to 4 questions and wait for the answers. Use it when a "
      "decision is theirs and no tool can settle it; a single open question "
      "belongs in your answer instead. Give each question a short header "
      "and 2 to 4 distinct options, recommended first; the user can always "
      "answer in their own words or attach an image instead. Set "
      "multi_select when the options are not exclusive. Options that differ "
      "in look can show an image you made or a monospace preview.",
      json::parse(R"json({"type":"object","properties":{
        "questions":{"type":"array","minItems":1,"maxItems":4,"items":{
          "type":"object","properties":{
            "question":{"type":"string"},
            "header":{"type":"string","description":"chip label, 12 characters at most"},
            "options":{"type":"array","minItems":2,"maxItems":4,"items":{
              "type":"object","properties":{
                "label":{"type":"string"},
                "description":{"type":"string","description":"the image's alt text"},
                "image":{"type":"string","description":"image file in the workspace"},
                "preview":{"type":"string","description":"40 lines at most"}},
              "required":["label"]}},
            "multi_select":{"type":"boolean"}},
          "required":["question","header","options"]}}},
        "required":["questions"]})json"),
      [ask = std::move(ask), image = std::move(image)](
          const json& a, const ToolContext& context) {
        const json& questions = a["questions"];
        std::string notes;
        bool eof = false;
        const json answers = json::parse(
            ask(ShownQuestions(questions, image, notes), &eof), nullptr,
            false);
        if (eof || !answers.is_array()) {
          return ToolSuccess(
              "The user did not answer; decide yourself or ask in your "
              "reply." +
              notes);
        }
        std::string out;
        for (size_t index = 0; index < questions.size(); ++index) {
          const json answer =
              index < answers.size() ? answers[index] : json::object();
          std::string chosen, labels;
          const auto add = [&chosen](const std::string& part) {
            chosen += (chosen.empty() ? "" : "; ") + part;
          };
          if (const json* choices = JsonArray(answer, "choices")) {
            for (const json& choice : *choices) {
              if (!choice.is_string()) continue;
              labels +=
                  (labels.empty() ? "" : ", ") + choice.get<std::string>();
            }
          }
          if (!labels.empty()) add(labels);
          const std::string other = JsonValue(answer, "other", "");
          if (!other.empty()) add("in their words: " + other);
          const std::string theirs = JsonValue(answer, "image", "");
          if (!theirs.empty() &&
              Attachments().Add(theirs, context.call_id).Ok()) {
            add("an image, readable in your next step");
          }
          out += JsonValue(questions[index], "question", "") + "\n→ " +
                 (chosen.empty() ? "(no answer)" : chosen) + "\n";
        }
        return ToolSuccess(out + notes);
      });
  tool.validate = [](const json& a) -> std::optional<ToolArgumentIssue> {
    if (auto issue = AskQuestionsIssue(JsonValue(a, "questions", json()))) {
      return ArgumentIssue("ask.questions", *issue, "questions");
    }
    return std::nullopt;
  };
  // Asking changes nothing and needs no capability: whoever may talk to the
  // user may ask them.
  tool.capabilities = 0;
  tool.intent = "ask";
  tool.summary = [](const json& a) {
    const json* questions = JsonArray(a, "questions");
    return questions && !questions->empty()
               ? JsonValue((*questions)[0], "header", "")
               : std::string();
  };
  tool.header = [](const json& a) {
    const json* questions = JsonArray(a, "questions");
    return json{{"verb", {"Asking", "Asked"}},
                {"target", questions && !questions->empty()
                               ? JsonValue((*questions)[0], "question", "")
                               : ""}};
  };
  return tool;
}

}  // namespace uagent
