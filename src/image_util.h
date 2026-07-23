#ifndef HAIKU_CLAUDE_CLI_IMAGE_UTIL_H
#define HAIKU_CLAUDE_CLI_IMAGE_UTIL_H

#include <string>

#include <nlohmann/json.hpp>

// image_util — pure helpers for attaching images to a Messages API turn.
// No BeAPI, no filesystem: base64 encoding, extension→media-type mapping,
// and building an `image` content block. Shared by the GUI (drag-drop) and
// the CLI (Tracker drop / --attach), and unit-tested in isolation.

namespace image {

using json = nlohmann::json;

// RFC 4648 base64 (standard alphabet with '+' '/' and '=' padding — the
// Anthropic image API wants this, not the URL-safe variant).
std::string Base64Encode(const std::string& in);

// Map a file path's extension to an Anthropic-supported image media type,
// or return an empty string if the extension is not a supported image
// (jpeg / png / gif / webp). Case-insensitive.
std::string MediaTypeForPath(const std::string& path);

// Build a base64 `image` content block from a media type and raw bytes.
//   { "type":"image",
//     "source":{ "type":"base64", "media_type":<mt>, "data":<b64> } }
json ImageBlock(const std::string& mediaType, const std::string& rawBytes);

} // namespace image

#endif // HAIKU_CLAUDE_CLI_IMAGE_UTIL_H
