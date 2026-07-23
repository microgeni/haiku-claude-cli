#include "image_util.h"

#include <cctype>

namespace image {

std::string Base64Encode(const std::string& in)
{
	static const char* kTable =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve(((in.size() + 2) / 3) * 4);
	size_t i = 0;
	for (; i + 2 < in.size(); i += 3) {
		const unsigned n = (static_cast<unsigned char>(in[i]) << 16)
		                 | (static_cast<unsigned char>(in[i + 1]) << 8)
		                 |  static_cast<unsigned char>(in[i + 2]);
		out += kTable[(n >> 18) & 63];
		out += kTable[(n >> 12) & 63];
		out += kTable[(n >> 6) & 63];
		out += kTable[n & 63];
	}
	if (i < in.size()) {
		unsigned n = static_cast<unsigned char>(in[i]) << 16;
		if (i + 1 < in.size())
			n |= static_cast<unsigned char>(in[i + 1]) << 8;
		out += kTable[(n >> 18) & 63];
		out += kTable[(n >> 12) & 63];
		out += (i + 1 < in.size()) ? kTable[(n >> 6) & 63] : '=';
		out += '=';
	}
	return out;
}

std::string MediaTypeForPath(const std::string& path)
{
	const auto dot = path.rfind('.');
	if (dot == std::string::npos) return {};
	std::string ext = path.substr(dot + 1);
	for (char& c : ext) c = static_cast<char>(std::tolower((unsigned char)c));
	if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
	if (ext == "png")                  return "image/png";
	if (ext == "gif")                  return "image/gif";
	if (ext == "webp")                 return "image/webp";
	return {};
}

json ImageBlock(const std::string& mediaType, const std::string& rawBytes)
{
	return {
		{"type", "image"},
		{"source", {
			{"type",       "base64"},
			{"media_type", mediaType},
			{"data",       Base64Encode(rawBytes)},
		}},
	};
}

} // namespace image
