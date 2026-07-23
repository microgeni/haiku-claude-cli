// Unit tests for image::* — the pure image-attachment helpers (base64,
// media-type mapping, image content block). No BeAPI, no filesystem.
//
// Build: see the `test-unit` target in the top-level Makefile.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../../src/image_util.h"

#include <string>

using image::Base64Encode;
using image::MediaTypeForPath;
using image::ImageBlock;

// ── Base64Encode ─────────────────────────────────────────────────────────────
// Test vectors from RFC 4648 §10.

TEST_CASE("Base64Encode: RFC 4648 test vectors") {
	CHECK(Base64Encode("")       == "");
	CHECK(Base64Encode("f")      == "Zg==");
	CHECK(Base64Encode("fo")     == "Zm8=");
	CHECK(Base64Encode("foo")    == "Zm9v");
	CHECK(Base64Encode("foob")   == "Zm9vYg==");
	CHECK(Base64Encode("fooba")  == "Zm9vYmE=");
	CHECK(Base64Encode("foobar") == "Zm9vYmFy");
}

TEST_CASE("Base64Encode: binary bytes (0x00, 0xFF) encode correctly") {
	std::string bytes;
	bytes.push_back('\x00');
	bytes.push_back('\xFF');
	bytes.push_back('\x10');
	// 0x00 0xFF 0x10 -> AP8Q
	CHECK(Base64Encode(bytes) == "AP8Q");
}

TEST_CASE("Base64Encode: standard alphabet uses + and / (not URL-safe)") {
	// 0xFB 0xFF 0xFE contains the sextets that map to '+' and '/'.
	std::string bytes;
	bytes.push_back('\xFB');
	bytes.push_back('\xFF');
	bytes.push_back('\xBF');
	const std::string out = Base64Encode(bytes);
	// Must contain a '+' or '/' — the standard alphabet, per the Anthropic API.
	CHECK((out.find('+') != std::string::npos || out.find('/') != std::string::npos));
	CHECK(out.find('-') == std::string::npos);  // not URL-safe
	CHECK(out.find('_') == std::string::npos);
}

// ── MediaTypeForPath ─────────────────────────────────────────────────────────

TEST_CASE("MediaTypeForPath: supported extensions map correctly") {
	CHECK(MediaTypeForPath("/a/b/photo.png")  == "image/png");
	CHECK(MediaTypeForPath("shot.jpg")        == "image/jpeg");
	CHECK(MediaTypeForPath("shot.jpeg")       == "image/jpeg");
	CHECK(MediaTypeForPath("anim.gif")        == "image/gif");
	CHECK(MediaTypeForPath("pic.webp")        == "image/webp");
}

TEST_CASE("MediaTypeForPath: case-insensitive") {
	CHECK(MediaTypeForPath("PHOTO.PNG")  == "image/png");
	CHECK(MediaTypeForPath("Shot.JPeG")  == "image/jpeg");
}

TEST_CASE("MediaTypeForPath: non-images and extensionless return empty") {
	CHECK(MediaTypeForPath("notes.txt")  == "");
	CHECK(MediaTypeForPath("main.cpp")   == "");
	CHECK(MediaTypeForPath("README")     == "");
	CHECK(MediaTypeForPath("")           == "");
	CHECK(MediaTypeForPath("archive.tar.gz") == "");
}

// ── ImageBlock ───────────────────────────────────────────────────────────────

TEST_CASE("ImageBlock: shape matches the Anthropic base64 image source") {
	const image::json b = ImageBlock("image/png", "foobar");
	CHECK(b["type"] == "image");
	CHECK(b["source"]["type"]       == "base64");
	CHECK(b["source"]["media_type"] == "image/png");
	CHECK(b["source"]["data"]       == "Zm9vYmFy");  // base64("foobar")
}
