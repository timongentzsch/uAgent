// Copyright 2026 Timon Gentzsch

#include "include/app/tool_categories.h"

#include <algorithm>
#include <set>
#include <string>

#include "include/core/private_store.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/core/time.h"

namespace uagent {
namespace {

constexpr int64_t kToolCategoryStoreVersion = 1;
constexpr size_t kToolCategoryStoreBytes = size_t{256} * 1024;
constexpr size_t kCategoryNameChars = 64;
constexpr size_t kCategoryIdChars = 16;
constexpr char kToolCategoryStoreFile[] = "tool-categories.json";

json EmptyStore() {
  return {{"version", kToolCategoryStoreVersion},
          {"categories", json::array()},
          {"assignments", json::object()}};
}

bool ValidStore(const json& store, std::string& error) {
  if (!store.is_object() ||
      JsonValue(store, "version", int64_t{0}) != kToolCategoryStoreVersion ||
      !store.contains("categories") || !store["categories"].is_array() ||
      !store.contains("assignments") || !store["assignments"].is_object()) {
    error = "tool category file is invalid";
    return false;
  }
  std::set<std::string> ids;
  for (const auto& category : store["categories"]) {
    const std::string id = JsonValue(category, "id", "");
    const std::string name = JsonValue(category, "name", "");
    if (id.empty() || id.size() > kCategoryIdChars || name.empty() ||
        name.size() > kCategoryNameChars || !ids.insert(id).second) {
      error = "tool category file contains an invalid category";
      return false;
    }
  }
  for (const auto& [tool, category] : store["assignments"].items()) {
    if (tool.empty() || tool.size() > kToolNameChars || !category.is_string() ||
        !ids.contains(category.get<std::string>())) {
      error = "tool category file contains an invalid assignment";
      return false;
    }
  }
  return true;
}

json Result(const json& store) {
  return {{"categories", store["categories"]},
          {"assignments", store["assignments"]}};
}

}  // namespace

json ToolCategoriesControl(const json& request) {
  std::string error;
  PrivateJsonStore file(kToolCategoryStoreFile, EmptyStore(),
                        kToolCategoryStoreBytes, error);
  if (!file.Ready() || !ValidStore(file.Data(), error)) {
    return {{"error", error}};
  }
  json& store = file.Data();
  const std::string action = JsonValue(request, "action", "list");
  if (action == "list") return Result(store);

  const std::string category_id = JsonValue(request, "category_id", "");
  if (action == "create") {
    const std::string name = Trim(JsonValue(request, "name", ""));
    if (name.empty() || name.size() > kCategoryNameChars) {
      return {{"error", "category name is empty or too long"}};
    }
    std::string id;
    do {
      id = HashHex(name + MakeSessionId()).substr(0, kCategoryIdChars);
    } while (std::any_of(store["categories"].begin(), store["categories"].end(),
                         [&](const json& category) {
                           return JsonValue(category, "id", "") == id;
                         }));
    store["categories"].push_back(
        {{"id", id}, {"name", name}, {"created", NowSeconds()}});
  } else if (action == "rename") {
    const std::string name = Trim(JsonValue(request, "name", ""));
    auto found =
        std::find_if(store["categories"].begin(), store["categories"].end(),
                     [&](const json& category) {
                       return JsonValue(category, "id", "") == category_id;
                     });
    if (found == store["categories"].end()) {
      return {{"error", "tool category not found"}};
    }
    if (name.empty() || name.size() > kCategoryNameChars) {
      return {{"error", "category name is empty or too long"}};
    }
    (*found)["name"] = name;
  } else if (action == "delete") {
    auto& categories = store["categories"];
    const size_t before = categories.size();
    categories.erase(std::remove_if(categories.begin(), categories.end(),
                                    [&](const json& category) {
                                      return JsonValue(category, "id", "") ==
                                             category_id;
                                    }),
                     categories.end());
    if (categories.size() == before) {
      return {{"error", "tool category not found"}};
    }
    auto& assignments = store["assignments"];
    for (auto it = assignments.begin(); it != assignments.end();) {
      if (it.value() == category_id) {
        it = assignments.erase(it);
      } else {
        ++it;
      }
    }
  } else if (action == "assign") {
    const std::string tool = JsonValue(request, "name", "");
    if (tool.empty() || tool.size() > kToolNameChars) {
      return {{"error", "tool name is empty or too long"}};
    }
    if (category_id.empty()) {
      store["assignments"].erase(tool);
    } else {
      bool found =
          std::any_of(store["categories"].begin(), store["categories"].end(),
                      [&](const json& category) {
                        return JsonValue(category, "id", "") == category_id;
                      });
      if (!found) return {{"error", "tool category not found"}};
      store["assignments"][tool] = category_id;
    }
  } else {
    return {{"error", "unknown tool category action"}};
  }
  if (!file.Save(error)) return {{"error", error}};
  return Result(store);
}

}  // namespace uagent
