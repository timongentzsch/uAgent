// Copyright 2026 Timon Gentzsch

#include "include/mcp/elicit.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include "include/core/strings.h"

namespace uagent {
namespace {
bool Strings(const json* values) {
  return values && !values->empty() &&
         std::ranges::all_of(
             *values, [](const json& value) { return value.is_string(); });
}

bool Titled(const json* values) {
  return values && !values->empty() &&
         std::ranges::all_of(*values, [](const json& value) {
           return JsonStringRef(value, "const") &&
                  (!value.contains("title") || value["title"].is_string());
         });
}

bool Count(const json& field, const char* key) {
  return !field.contains(key) || field[key].is_number_unsigned();
}

// Both bounds, when set, in order.
bool Ordered(const json& field, const char* low, const char* high) {
  return !field.contains(low) || !field.contains(high) ||
         !field[low].is_number() || !field[high].is_number() ||
         field[low].get<double>() <= field[high].get<double>();
}

bool Typed(const json& field, const std::string& type) {
  if (!field.contains("default")) return true;
  const json& value = field["default"];
  if (type == "integer") return value.is_number_integer();
  if (type == "number") return value.is_number();
  if (type == "boolean") return value.is_boolean();
  if (type == "array") {
    return value.is_array() && std::ranges::all_of(value, [](const json& one) {
             return one.is_string();
           });
  }
  return value.is_string();
}

// "at least 2 and at most 5": the bounds a field sets.
std::string Between(const json& field, const char* low, const char* high) {
  std::string out;
  if (field.contains(low)) out = "at least " + field[low].dump();
  if (field.contains(high)) {
    out += (out.empty() ? "" : " and ") + std::string("at most ") +
           field[high].dump();
  }
  return out;
}

std::optional<std::string> FieldShapeIssue(const json& field) {
  const std::string type = JsonValue(field, "type", "");
  bool shaped = false;
  if (type == "string") {
    const std::string format = JsonValue(field, "format", "");
    shaped = Count(field, "minLength") && Count(field, "maxLength") &&
             (format.empty() || format == "email" || format == "uri" ||
              format == "date" || format == "date-time") &&
             (!field.contains("enum") || Strings(JsonArray(field, "enum"))) &&
             (!field.contains("oneOf") || Titled(JsonArray(field, "oneOf")));
  } else if (type == "number" || type == "integer") {
    shaped = (!field.contains("minimum") || field["minimum"].is_number()) &&
             (!field.contains("maximum") || field["maximum"].is_number());
  } else if (type == "boolean") {
    shaped = true;
  } else if (type == "array") {
    const json items = JsonValue(field, "items", json::object());
    shaped = Count(field, "minItems") && Count(field, "maxItems") &&
             (Strings(JsonArray(items, "enum")) ||
              Titled(JsonArray(items, "anyOf")));
  }
  const bool labelled =
      (!field.contains("title") || field["title"].is_string()) &&
      (!field.contains("description") || field["description"].is_string());
  if (shaped && labelled && Typed(field, type) &&
      Ordered(field, "minimum", "maximum") &&
      Ordered(field, "minLength", "maxLength") &&
      Ordered(field, "minItems", "maxItems")) {
    return std::nullopt;
  }
  if (shaped) return "has a title, default or bounds that do not fit it";
  return type == "object" || type == "array"
             ? "is nested; a form takes only flat fields"
             : "is not a string, number, boolean or choice field";
}

std::optional<std::string> FieldIssue(const json& field, const json& value) {
  const std::string type = JsonValue(field, "type", "");
  const json choices = ElicitChoices(field);
  const auto offered = [&](const json& one) {
    return std::ranges::any_of(
        choices, [&](const json& choice) { return choice["value"] == one; });
  };
  if (type == "array") {
    if (!value.is_array() || !std::ranges::all_of(value, offered)) {
      return "choose from the listed options";
    }
    if (value.size() < JsonValue(field, "minItems", size_t{0}) ||
        value.size() > JsonValue(field, "maxItems", value.size())) {
      return "choose " + Between(field, "minItems", "maxItems") + " options";
    }
    return std::nullopt;
  }
  if (!choices.empty()) {
    if (offered(value)) return std::nullopt;
    return "choose one of the listed options";
  }
  if (type == "boolean") {
    if (value.is_boolean()) return std::nullopt;
    return "answer yes or no";
  }
  if (type == "number" || type == "integer") {
    if (!value.is_number() ||
        (type == "integer" && !value.is_number_integer())) {
      return "enter " +
             std::string(type == "integer" ? "a whole number" : "a number");
    }
    const double number = value.get<double>();
    if ((field.contains("minimum") &&
         number < field["minimum"].get<double>()) ||
        (field.contains("maximum") &&
         number > field["maximum"].get<double>())) {
      return "enter a number " + Between(field, "minimum", "maximum");
    }
    return std::nullopt;
  }
  if (!value.is_string()) return "enter text";
  const std::string text = value.get<std::string>();
  const size_t length = Utf8Chars(text);
  if (length < JsonValue(field, "minLength", size_t{0}) ||
      length > JsonValue(field, "maxLength", length)) {
    return "enter " + Between(field, "minLength", "maxLength") + " characters";
  }
  const std::string format = JsonValue(field, "format", "");
  const size_t at = text.find('@');
  const bool digits =
      text.size() >= 10 &&
      std::ranges::all_of(
          std::string_view(text).substr(0, 10),
          [](char ch) { return ch == '-' || std::isdigit(Byte(ch)); }) &&
      text[4] == '-' && text[7] == '-';
  if ((format == "email" &&
       (at == 0 || at == std::string::npos || at + 1 == text.size() ||
        text.find(' ') != std::string::npos)) ||
      (format == "uri" &&
       (text.find(':') == std::string::npos || !std::isalpha(Byte(text[0])))) ||
      (format == "date" && (!digits || text.size() != 10)) ||
      (format == "date-time" &&
       (!digits || text.size() < 16 || (text[10] != 'T' && text[10] != ' ')))) {
    return "enter a valid " + format;
  }
  return std::nullopt;
}

// An http(s) URL's host, or nothing for any other URL: userinfo, a backslash
// (a separator to browsers) or an escape would let the host shown differ from
// the one opened.
std::optional<std::string> UrlTarget(const std::string& url) {
  const std::string lower = AsciiLower(url);
  size_t start = lower.starts_with("https://")  ? 8
                 : lower.starts_with("http://") ? 7
                                                : 0;
  if (start == 0 || std::ranges::any_of(url, [](char ch) {
        return Byte(ch) <= 0x20 || Byte(ch) == 0x7f;
      })) {
    return std::nullopt;
  }
  const std::string authority =
      lower.substr(start, lower.find_first_of("/?#", start) - start);
  const size_t port = authority.starts_with('[') ? authority.find(']') + 1
                                                 : authority.find(':');
  std::string host = authority.substr(0, port);
  if (host.empty() || authority.find_first_of("@\\%") != std::string::npos ||
      std::ranges::any_of(authority,
                          [](char ch) { return Byte(ch) >= 0x80; })) {
    return std::nullopt;
  }
  return host;
}
}  // namespace

json ElicitChoices(const json& field) {
  const json items = JsonValue(field, "items", json::object());
  json choices = json::array();
  const json* titled = JsonArray(field, "oneOf");
  if (!titled) titled = JsonArray(items, "anyOf");
  if (titled) {
    for (const json& choice : *titled) {
      const std::string value = JsonValue(choice, "const", "");
      choices.push_back(
          {{"value", value}, {"label", JsonValue(choice, "title", value)}});
    }
    return choices;
  }
  const json* plain = JsonArray(field, "enum");
  if (!plain) plain = JsonArray(items, "enum");
  for (size_t index = 0; plain && index < plain->size(); ++index) {
    choices.push_back({{"value", (*plain)[index]}, {"label", (*plain)[index]}});
  }
  return choices;
}

std::optional<std::string> ElicitSchemaIssue(const json& schema) {
  const json* properties = JsonObject(schema, "properties");
  if (JsonValue(schema, "type", "") != "object" || !properties) {
    return "its form is not an object with properties";
  }
  for (const auto& [name, field] : properties->items()) {
    if (auto issue = FieldShapeIssue(field)) {
      return "its field `" + name + "` " + *issue;
    }
  }
  for (const json& name : JsonValue(schema, "required", json::array())) {
    if (!name.is_string() || !properties->contains(name.get<std::string>())) {
      return "it requires a field it does not define";
    }
  }
  return std::nullopt;
}

std::optional<std::string> ElicitContentIssue(const json& schema,
                                              const json& content) {
  if (!content.is_object()) return "the answers must be an object";
  const json properties = JsonValue(schema, "properties", json::object());
  for (const json& name : JsonValue(schema, "required", json::array())) {
    if (name.is_string() && !content.contains(name.get<std::string>())) {
      return name.get<std::string>() + ": required";
    }
  }
  for (const auto& [name, value] : content.items()) {
    if (!properties.contains(name)) return name + ": not a field of this form";
    if (auto issue = FieldIssue(properties[name], value)) {
      return name + ": " + *issue;
    }
  }
  return std::nullopt;
}

bool ElicitParseField(const json& field, const std::string& text, json& value,
                      std::string& error) {
  const std::string typed = Trim(text);
  const std::string type = JsonValue(field, "type", "");
  const json choices = ElicitChoices(field);
  value = nullptr;
  if (typed.empty()) {
    value = JsonValue(field, "default", json(nullptr));
  } else if (!choices.empty()) {
    const auto choose = [&](const std::string& part) {
      int64_t number = 0;
      if (ParseInt64(part.c_str(), number) && number >= 1 &&
          number <= static_cast<int64_t>(choices.size())) {
        return choices[static_cast<size_t>(number - 1)]["value"];
      }
      for (const json& choice : choices) {
        if (choice["value"] == part || choice["label"] == part) {
          return choice["value"];
        }
      }
      return json(part);
    };
    if (type == "array") {
      value = json::array();
      for (const auto part : std::views::split(typed, ',')) {
        const std::string one = Trim(std::string(part.begin(), part.end()));
        if (!one.empty()) value.push_back(choose(one));
      }
    } else {
      value = choose(typed);
    }
  } else if (type == "boolean") {
    bool yes = false;
    value = ParseBool(typed, yes) ? json(yes) : json(typed);
  } else if (type == "number" || type == "integer") {
    double number = 0;
    int64_t whole = 0;
    value = ParseInt64(typed.c_str(), whole)           ? json(whole)
            : ParseFiniteDouble(typed.c_str(), number) ? json(number)
                                                       : json(typed);
  } else {
    value = typed;
  }
  if (value.is_null()) return true;
  auto issue = FieldIssue(field, value);
  if (issue) error = *issue;
  return !issue;
}

std::optional<std::string> ElicitReplyIssue(const json& shown,
                                            const json& reply) {
  const std::string action = JsonValue(reply, "action", "");
  if (action != "accept" && action != "decline" && action != "cancel") {
    return "answer with accept, decline or cancel";
  }
  const bool form = JsonValue(shown, "mode", "") == "form";
  if (action == "accept" && form) {
    return ElicitContentIssue(JsonValue(shown, "requestedSchema", json()),
                              JsonValue(reply, "content", json()));
  }
  if (reply.contains("content")) return "only an accepted form has answers";
  return std::nullopt;
}

json McpElicit(const McpServer& server, const json& params, std::string& note) {
  const std::string mode = JsonValue(params, "mode", "form");
  json shown = {{"server", server.name},
                {"mode", mode},
                {"message", JsonValue(params, "message", "")}};
  std::optional<std::string> issue;
  if (mode == "form") {
    shown["requestedSchema"] = JsonValue(params, "requestedSchema", json());
    issue = ElicitSchemaIssue(shown["requestedSchema"]);
    if (!issue) {
      for (auto& [name, field] :
           shown["requestedSchema"]["properties"].items()) {
        field["choices"] = ElicitChoices(field);
      }
    }
  } else if (mode == "url") {
    shown["url"] = JsonValue(params, "url", "");
    if (auto host = UrlTarget(shown["url"])) {
      shown["host"] = *host;
      shown["punycode"] =
          host->starts_with("xn--") || host->find(".xn--") != std::string::npos;
    } else {
      issue = "its address is not a plain http or https URL";
    }
  } else {
    issue = "its mode `" + mode + "` is neither form nor url";
  }
  if (JsonValue(shown, "message", "").empty()) issue = "it gives no message";
  if (issue) {
    note +=
        "[declined " + server.name + "'s request for input: " + *issue + "]\n";
    return {{"action", "decline"}};
  }
  bool eof = false;
  // Every reply has passed ElicitReplyIssue where it was given.
  const json reply = json::parse(server.elicit(shown, &eof), nullptr, false);
  if (eof || reply.is_discarded()) return {{"action", "cancel"}};
  return JsonPick(reply, {"action", "content"});
}

bool McpInputResponses(const McpServer& server, const json& requests,
                       json& responses, bool& wait, std::string& note) {
  size_t shown = 0;
  bool ended = false;
  for (const auto& [key, input] : requests.items()) {
    const std::string method = JsonValue(input, "method", "");
    const json params = JsonValue(input, "params", json::object());
    if (method == "roots/list") {
      responses[key] = {{"roots", server.roots}};
      continue;
    }
    if (method != "elicitation/create" || !server.elicit) return false;
    if (ended) {
      responses[key] = {{"action", "cancel"}};
    } else if (++shown > kMcpElicitationsPerRound) {
      note += "[declined " + server.name + "'s requests for input beyond " +
              std::to_string(kMcpElicitationsPerRound) + " at once]\n";
      responses[key] = {{"action", "decline"}};
    } else {
      responses[key] = McpElicit(server, params, note);
      const bool accepted = responses[key]["action"] == "accept";
      ended = !accepted;
      wait = wait || (accepted && JsonValue(params, "mode", "") == "url");
    }
  }
  return true;
}

}  // namespace uagent
