// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_WEB_ASSETS_H_
#define UAGENT_INCLUDE_WEB_ASSETS_H_
#include <span>
#include <string_view>
namespace uagent::web {
struct Asset {
  std::string_view path, mime;
  const unsigned char* data;
  size_t size;
};
std::span<const Asset> Assets();
}  // namespace uagent::web
#endif  // UAGENT_INCLUDE_WEB_ASSETS_H_
