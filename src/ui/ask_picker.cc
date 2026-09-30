// Copyright 2026 Timon Gentzsch

#include "include/ui/ask_picker.h"

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "include/core/strings.h"
#include "include/core/style.h"
#include "include/core/term.h"
#include "include/ui/input_decoder.h"
#include "include/ui/live_region.h"

namespace uagent {
namespace {
using Live = std::function<bool()>;

// Decoded keys from stdin. Empty once stdin closes or the decision is gone.
class Keys {
 public:
  std::optional<TerminalInputToken> Next(const Live& live) {
    for (;;) {
      const auto deadline = decoder_.WakeDeadline();
      const bool expired =
          deadline && std::chrono::steady_clock::now() >= *deadline;
      if (auto token = decoder_.Next(expired)) return token;
      if (!live()) return std::nullopt;
      pollfd in{STDIN_FILENO, POLLIN, 0};
      if (poll(&in, 1, deadline ? 20 : 200) <= 0) continue;
      unsigned char bytes[1024];
      const ssize_t count = read(STDIN_FILENO, bytes, sizeof bytes);
      if (count == 0) return std::nullopt;
      if (count > 0) decoder_.Feed(bytes, static_cast<size_t>(count));
    }
  }

 private:
  TerminalInputDecoder decoder_;
};

// The picker stands in for the composer while it asks.
void Draw(LiveRegion& region, std::string text) {
  region.SetOverlay(std::move(text));
  region.Flush();
}

bool Enter(const TerminalInputToken& token) {
  return token.kind == TerminalInputTokenKind::kText &&
         (token.text == "\r" || token.text == "\n");
}

// One line of the person's own words under `lead`; empty when escaped.
std::optional<std::string> ReadLine(Keys& keys, LiveRegion& region,
                                    const std::string& lead, const Live& live) {
  std::string line;
  for (;;) {
    Draw(region, lead + TerminalSafe(line));
    auto token = keys.Next(live);
    if (!token || token->kind == TerminalInputTokenKind::kEscape) {
      return std::nullopt;
    }
    if (Enter(*token)) return Trim(line);
    if (token->kind == TerminalInputTokenKind::kSequence) continue;
    for (char ch : token->text) {
      if (ch == 0x7f || ch == 0x08) {
        line.erase(Utf8BoundaryBefore(line, line.size()));
      } else if (static_cast<unsigned char>(ch) >= 0x20) {
        line += ch;
      }
    }
  }
}

struct Answer {
  std::vector<bool> chosen;
  std::string other, image;
};

std::string Render(const json& question, const Answer& answer, size_t cursor,
                   const std::string& note) {
  const json& options = question["options"];
  const bool multi = JsonValue(question, "multi_select", false);
  const size_t width = TerminalWidth();
  std::string out = std::string(BOLD()) +
                    TerminalSafe(JsonValue(question, "header", "")) + RST() +
                    AsciiGlyphs(" · ") +
                    TerminalSafe(JsonValue(question, "question", "")) + "\n";
  for (size_t index = 0; index <= options.size(); ++index) {
    const bool other = index == options.size();
    // The composer's prompt glyph marks the row a key acts on.
    std::string row = index == cursor ? "> " : "  ";
    if (multi)
      row += (other ? !answer.other.empty() : answer.chosen[index]) ? "[x] "
                                                                    : "[ ] ";
    row += std::to_string(index + 1) + ". ";
    std::string description;
    if (other) {
      row += answer.other.empty() ? AsciiGlyphs("Other…")
                                  : "Other: " + TerminalSafe(answer.other);
    } else {
      row += TerminalSafe(JsonValue(options[index], "label", ""));
      description = TerminalSafe(JsonValue(options[index], "description", ""));
    }
    // Safe and cut as plain text, then styled: an escape is not width, and
    // TerminalSafe would spell out the dim instead of letting it apply.
    const std::string lead = row + " ";
    row = DisplayTrunc(
        description.empty() ? row : lead + AsciiGlyphs("— ") + description,
        width);
    if (!description.empty() && row.starts_with(lead)) {
      row = lead + DIM() + row.substr(lead.size()) + RST();
    }
    out += row + "\n";
  }
  // The focused option as the agent showed it: its image, by path, and its
  // preview, each line made safe and cut to the terminal.
  if (cursor < options.size()) {
    const json& focused = options[cursor];
    const std::string shown =
        JsonValue(JsonValue(focused, "image", json::object()), "path", "");
    if (!shown.empty()) {
      out += std::string(DIM()) +
             DisplayTrunc("  image: " + TerminalSafe(shown), width) + RST() +
             "\n";
    }
    std::string preview = JsonValue(focused, "preview", "");
    while (!preview.empty()) {
      const size_t end = preview.find('\n');
      out += "  " +
             DisplayTrunc(TerminalSafe(preview.substr(0, end)), width - 2) +
             "\n";
      preview = end == std::string::npos ? "" : preview.substr(end + 1);
    }
  }
  if (!answer.image.empty()) {
    out += "  image: " + TerminalSafe(answer.image) + "\n";
  }
  out += std::string(DIM()) +
         (note.empty() ? AsciiGlyphs(std::string("↑↓ move · ") +
                                     (multi ? "Space toggle · Enter done"
                                            : "Enter choose") +
                                     " · i image · Esc cancel")
                       : TerminalSafe(note)) +
         RST();
  return out;
}

bool Answered(const Answer& answer) {
  return !answer.other.empty() ||
         std::ranges::find(answer.chosen, true) != answer.chosen.end();
}
}  // namespace

std::string AskAnswersFromLine(const json& questions, const std::string& line) {
  // The part up to `separator` from `at`, advancing past it.
  const auto take = [](const std::string& text, size_t& at, char separator) {
    const size_t end = std::min(text.find(separator, at), text.size());
    std::string part = at < text.size() ? Trim(text.substr(at, end - at)) : "";
    at = end + 1;
    return part;
  };
  json answers = json::array();
  size_t at = 0;
  for (const json& question : questions) {
    const std::string part = take(line, at, ';');
    const json& options = question["options"];
    json choices = json::array();
    for (size_t from = 0; !part.empty() && from <= part.size();) {
      int64_t number = 0;
      if (!ParseInt64(take(part, from, ',').c_str(), number) || number < 1 ||
          number > static_cast<int64_t>(options.size())) {
        choices = nullptr;
        break;
      }
      choices.push_back(
          JsonValue(options[static_cast<size_t>(number - 1)], "label", ""));
    }
    answers.push_back(choices.is_array() && !choices.empty()
                          ? json{{"choices", choices}, {"other", ""}}
                          : json{{"choices", json::array()}, {"other", part}});
  }
  return JsonDump(answers);
}

json PickAskAnswers(const json& questions, LiveRegion& region,
                    const Live& live) {
  Keys keys;
  // Each poll redraws what a resize reflowed.
  const Live awake = [&] {
    region.Flush();
    return live();
  };
  json answers = json::array(), attachments = json::array();
  for (size_t number = 0; number < questions.size(); ++number) {
    const json& question = questions[number];
    const json& options = question["options"];
    const bool multi = JsonValue(question, "multi_select", false);
    Answer answer{std::vector<bool>(options.size(), false), "", ""};
    size_t cursor = 0;
    std::string note;
    bool done = false;
    while (!done) {
      Draw(region, Render(question, answer, cursor, note));
      note.clear();
      auto token = keys.Next(awake);
      if (!token || token->kind == TerminalInputTokenKind::kEscape) {
        region.SetOverlay("");
        return nullptr;
      }
      const std::string key = token->text;
      const bool on_other = cursor == options.size();
      if (key == "\033[A" || key == "\033OA" || key == "k") {
        cursor = cursor == 0 ? options.size() : cursor - 1;
      } else if (key == "\033[B" || key == "\033OB" || key == "j") {
        cursor = cursor == options.size() ? 0 : cursor + 1;
      } else if (key.size() == 1 && key[0] >= '1' &&
                 static_cast<size_t>(key[0] - '1') <= options.size()) {
        cursor = static_cast<size_t>(key[0] - '1');
      } else if (key == "i") {
        auto path = ReadLine(keys, region, "Image path: ", awake);
        std::error_code ec;
        const std::string file = path ? Unquote(*path) : "";
        if (!file.empty() && std::filesystem::is_regular_file(file, ec)) {
          answer.image = file;
        } else if (path && !path->empty()) {
          note = "not a file: " + *path;
        }
      } else if ((key == " " && multi) || (Enter(*token) && on_other)) {
        if (on_other) {
          auto words = ReadLine(keys, region, "In your words: ", awake);
          if (words) answer.other = *words;
          if (!multi && !answer.other.empty()) done = true;
        } else {
          answer.chosen[cursor] = !answer.chosen[cursor];
        }
      } else if (Enter(*token)) {
        if (!multi) {
          answer.chosen.assign(options.size(), false);
          answer.chosen[cursor] = true;
        } else if (!Answered(answer)) {
          answer.chosen[cursor] = true;
        }
        done = true;
      }
    }
    json entry = {{"choices", json::array()}, {"other", answer.other}};
    std::string shown;
    for (size_t index = 0; index < options.size(); ++index) {
      if (!answer.chosen[index]) continue;
      const std::string label = JsonValue(options[index], "label", "");
      entry["choices"].push_back(label);
      shown += (shown.empty() ? "" : ", ") + label;
    }
    if (!answer.other.empty()) {
      shown += (shown.empty() ? "" : "; ") + answer.other;
    }
    if (!answer.image.empty()) {
      const std::string id = "ask-" + std::to_string(number);
      entry["attachment_id"] = id;
      attachments.push_back({{"path", answer.image}, {"id", id}});
    }
    answers.push_back(std::move(entry));
    region.SetOverlay("");
    region.Commit(
        Note(Tone::kNeutral, TerminalSafe(JsonValue(question, "question", "")) +
                                 " → " + TerminalSafe(shown)));
  }
  return {{"text", JsonDump(answers)}, {"attachments", attachments}};
}

}  // namespace uagent
