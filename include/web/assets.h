// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_WEB_ASSETS_H_
#define UAGENT_INCLUDE_WEB_ASSETS_H_
// The embedded web bundle: one entry per file, gzipped at build time.

#include <span>
#include <string_view>
namespace uagent::web {
struct Asset {
  std::string_view path, mime;
  const unsigned char* data;
  size_t size;
  bool gzip;              // data is the gzip of the asset
  size_t plain_size;      // the asset's bytes, whether or not data is gzipped
  std::string_view etag;  // a hash of the asset's bytes
};
std::span<const Asset> Assets();
}  // namespace uagent::web
#endif  // UAGENT_INCLUDE_WEB_ASSETS_H_
