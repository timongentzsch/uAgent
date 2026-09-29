// Copyright 2026 Timon Gentzsch

#include "include/tools/ask.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "include/core/strings.h"
#include "include/media/attachments.h"

namespace uagent {

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
    }
  }
  return std::nullopt;
}

Tool AskTool(AskPerson ask) {
  Tool tool = MakeTool(
      "ask",
      "Ask the user 1 to 4 questions and wait for the answers. Use it when a "
      "decision is theirs and no tool can settle it; a single open question "
      "belongs in your answer instead. Give each question a short header "
      "and 2 to 4 distinct options, recommended first; the user can always "
      "answer in their own words or attach an image instead. Set "
      "multi_select when the options are not exclusive.",
      json::parse(R"json({"type":"object","properties":{
        "questions":{"type":"array","minItems":1,"maxItems":4,"items":{
          "type":"object","properties":{
            "question":{"type":"string"},
            "header":{"type":"string","description":"chip label, 12 characters at most"},
            "options":{"type":"array","minItems":2,"maxItems":4,"items":{
              "type":"object","properties":{
                "label":{"type":"string"},
                "description":{"type":"string"}},
              "required":["label"]}},
            "multi_select":{"type":"boolean"}},
          "required":["question","header","options"]}}},
        "required":["questions"]})json"),
      [ask = std::move(ask)](const json& a, const ToolContext& context) {
        const json& questions = a["questions"];
        bool eof = false;
        const json answers =
            json::parse(ask(questions, &eof), nullptr, false);
        if (eof || !answers.is_array()) {
          return ToolSuccess(
              "The user did not answer; decide yourself or ask in your "
              "reply.");
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
          const std::string image = JsonValue(answer, "image", "");
          if (!image.empty() &&
              Attachments().Add(image, context.call_id).Ok()) {
            add("an image, readable in your next step");
          }
          out += JsonValue(questions[index], "question", "") + "\n→ " +
                 (chosen.empty() ? "(no answer)" : chosen) + "\n";
        }
        return ToolSuccess(out);
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
