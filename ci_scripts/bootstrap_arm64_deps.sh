#!/bin/sh
#
# bootstrap_arm64_deps.sh — stage the third-party dependencies needed to
# cross-compile the Claude CLI for Haiku/arm64 (Raspberry Pi 5).
#
# There is no arm64 HaikuPorts repository yet — https://eu.hpkg.haiku-os.org
# serves an empty index for arm64 — so the only prebuilt arm64 binaries
# available are the bootstrap packages Haiku's own build downloads. That set
# has curl and OpenSSL but no libedit, so libedit is cross-built from source
# here against the arm64 ncurses in the cross-tools sysroot.
#
# The result is a small prefix:
#
#     $DEPS/include/{curl,editline,nlohmann,yaml-cpp}/…   headers
#     $DEPS/include/histedit.h
#     $DEPS/lib/libcurl.a                        static, from bootstrap hpkg
#     $DEPS/lib/libedit.a                        static, built here
#     $DEPS/lib/libyaml-cpp.a                    static, built here (GUI)
#
# which `make ARCH=arm64` consumes via ARM64_DEPS. OpenSSL is taken straight
# from the cross sysroot (it ships libssl.a/libcrypto.a), and nlohmann/json
# is header-only so the host copy is architecture-independent.
#
# Usage:  sh ci_scripts/bootstrap_arm64_deps.sh
#
# Override the locations with environment variables if your tree differs:
#   HAIKU_ARM64_TREE  Haiku arm64 build tree (has cross-tools + download/)
#   DEPS              destination prefix

set -e

HAIKU_ARM64_TREE=${HAIKU_ARM64_TREE:-/Data/Code/Repos/haiku/generated.arm64}
DEPS=${DEPS:-/Data/Code/cross/haiku-arm64-deps}
HAIKUPORTS=${HAIKUPORTS:-/Data/Code/Repos/haikuports}

CROSS_TOOLS="$HAIKU_ARM64_TREE/cross-tools-arm64"
SYSROOT="$CROSS_TOOLS/sysroot/boot/system"
WORK=${WORK:-/tmp/haiku-arm64-deps-work}

LIBEDIT_VER=20230828-3.1
LIBEDIT_SHA=4ee8182b6e569290e7d1f44f0f78dac8716b35f656b76528f699c69c98814dad
LIBEDIT_URL="http://thrysoee.dk/editline/libedit-$LIBEDIT_VER.tar.gz"
LIBEDIT_PATCHSET="$HAIKUPORTS/sys-libs/libedit/patches/libedit-20230828_3.1.patchset"

# Matches the yaml_cpp0.8 recipe, i.e. the version the native x86_64 build
# links against.
YAMLCPP_VER=0.8.0
YAMLCPP_SHA=fbe74bbdcee21d656715688706da3c8becfd946d92cd44705cc6098bb23b3a16
YAMLCPP_URL="https://github.com/jbeder/yaml-cpp/archive/refs/tags/$YAMLCPP_VER.tar.gz"

TOOLCHAIN_FILE=${TOOLCHAIN_FILE:-$(cd "$(dirname "$0")" && pwd)/haiku-arm64-toolchain.cmake}

say() { printf '\n=== %s ===\n' "$1"; }

# --- sanity checks ---------------------------------------------------------

[ -x "$CROSS_TOOLS/bin/aarch64-unknown-haiku-gcc" ] || {
	echo "error: no aarch64 cross-tools at $CROSS_TOOLS" >&2
	echo "       build them with Haiku's configure --cross-tools-source … --build-cross-tools arm64" >&2
	exit 1
}

command -v cmake >/dev/null 2>&1 || {
	echo "error: cmake is required to cross-build yaml-cpp" >&2
	exit 1
}

[ -f "$TOOLCHAIN_FILE" ] || {
	echo "error: CMake toolchain file not found at $TOOLCHAIN_FILE" >&2
	exit 1
}

mkdir -p "$DEPS/include" "$DEPS/lib" "$WORK"

# --- curl: headers + static lib from the arm64 bootstrap package -----------

say "staging libcurl from the arm64 bootstrap package"
CURL_HPKG=$(ls "$HAIKU_ARM64_TREE"/download/curl-*-arm64.hpkg 2>/dev/null | head -1)
[ -n "$CURL_HPKG" ] || { echo "error: no arm64 curl hpkg in $HAIKU_ARM64_TREE/download" >&2; exit 1; }
echo "  $CURL_HPKG"
rm -rf "$WORK/curl" && mkdir -p "$WORK/curl"
(cd "$WORK/curl" && package extract "$CURL_HPKG")
mkdir -p "$DEPS/include/curl"
cp "$WORK"/curl/develop/headers/*.h "$DEPS/include/curl/"
cp "$WORK"/curl/develop/lib/libcurl.a "$DEPS/lib/"

# --- nlohmann/json: header-only, architecture-independent ------------------

say "staging nlohmann/json headers"
JSON_INC=$(pkg-config --variable=includedir nlohmann_json 2>/dev/null || true)
[ -n "$JSON_INC" ] && [ -d "$JSON_INC/nlohmann" ] || {
	echo "error: nlohmann_json headers not found on the host (pkg-config)" >&2
	exit 1
}
rm -rf "$DEPS/include/nlohmann"
cp -r "$JSON_INC/nlohmann" "$DEPS/include/"

# --- libedit: cross-build from source --------------------------------------

say "cross-building libedit $LIBEDIT_VER for arm64"
cd "$WORK"
[ -f "libedit-$LIBEDIT_VER.tar.gz" ] || curl -sL -o "libedit-$LIBEDIT_VER.tar.gz" "$LIBEDIT_URL"

GOT=$(sha256sum "libedit-$LIBEDIT_VER.tar.gz" | cut -d' ' -f1)
[ "$GOT" = "$LIBEDIT_SHA" ] || {
	echo "error: libedit checksum mismatch" >&2
	echo "  expected $LIBEDIT_SHA" >&2
	echo "  got      $GOT" >&2
	exit 1
}

rm -rf "libedit-$LIBEDIT_VER"
tar xzf "libedit-$LIBEDIT_VER.tar.gz"
cd "libedit-$LIBEDIT_VER"

# Haiku's libedit port needs the HaikuPorts patchset (sys/ttydefaults.h,
# missing headers, wide-char fixes). configure.ac is patched, hence the
# autoreconf below.
if [ -f "$LIBEDIT_PATCHSET" ]; then
	echo "  applying $LIBEDIT_PATCHSET"
	git init -q . 2>/dev/null || true
	git apply "$LIBEDIT_PATCHSET"
else
	echo "warning: patchset not found at $LIBEDIT_PATCHSET — building unpatched" >&2
fi

autoreconf --force --install >/dev/null 2>&1

PATH="$CROSS_TOOLS/bin:$PATH"; export PATH
CC=aarch64-unknown-haiku-gcc;    export CC
CXX=aarch64-unknown-haiku-g++;   export CXX
AR=aarch64-unknown-haiku-ar;     export AR
RANLIB=aarch64-unknown-haiku-ranlib; export RANLIB
CFLAGS="-O2 -I$SYSROOT/develop/headers -I$SYSROOT/develop/headers/ncursesw"; export CFLAGS
LDFLAGS="-L$SYSROOT/develop/lib"; export LDFLAGS
LIBS="-lncursesw"; export LIBS

./configure --host=aarch64-unknown-haiku --build=x86_64-unknown-haiku \
	--prefix=/boot/system --enable-examples=no --enable-widec \
	--disable-shared --enable-static >/dev/null

make -j"$(nproc 2>/dev/null || echo 4)" >/dev/null

# `make install` trips over the man pages, so install the two artefacts by
# hand — a static archive and its two public headers is all we need.
mkdir -p "$DEPS/include/editline"
cp src/.libs/libedit.a       "$DEPS/lib/"
cp src/editline/readline.h   "$DEPS/include/editline/"
cp src/histedit.h            "$DEPS/include/"

# --- yaml-cpp: cross-build from source (needed by the GUI) -----------------

say "cross-building yaml-cpp $YAMLCPP_VER for arm64"
cd "$WORK"
[ -f "yaml-cpp-$YAMLCPP_VER.tar.gz" ] || curl -sL -o "yaml-cpp-$YAMLCPP_VER.tar.gz" "$YAMLCPP_URL"

GOT=$(sha256sum "yaml-cpp-$YAMLCPP_VER.tar.gz" | cut -d' ' -f1)
[ "$GOT" = "$YAMLCPP_SHA" ] || {
	echo "error: yaml-cpp checksum mismatch" >&2
	echo "  expected $YAMLCPP_SHA" >&2
	echo "  got      $GOT" >&2
	exit 1
}

rm -rf "yaml-cpp-$YAMLCPP_VER"
tar xzf "yaml-cpp-$YAMLCPP_VER.tar.gz"
cd "yaml-cpp-$YAMLCPP_VER"

# The HaikuPorts patchset only rewires the bundled gtest, and tests are off
# here, so it is not applied. CMAKE_POLICY_VERSION_MINIMUM is needed because
# yaml-cpp 0.8.0 declares a cmake_minimum_required that modern CMake rejects.
mkdir -p build-arm64 && cd build-arm64
cmake -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
	-DYAML_BUILD_SHARED_LIBS=OFF \
	-DYAML_CPP_BUILD_TESTS=OFF \
	-DYAML_CPP_BUILD_TOOLS=OFF \
	-DYAML_CPP_BUILD_CONTRIB=OFF \
	.. >/dev/null

make -j"$(nproc 2>/dev/null || echo 4)" >/dev/null

cp libyaml-cpp.a "$DEPS/lib/"
rm -rf "$DEPS/include/yaml-cpp"
cp -r ../include/yaml-cpp "$DEPS/include/"

# --- verify ----------------------------------------------------------------

say "verifying staged archives are aarch64"
for a in "$DEPS/lib/libcurl.a" "$DEPS/lib/libedit.a" "$DEPS/lib/libyaml-cpp.a"; do
	member=$("$CROSS_TOOLS/bin/aarch64-unknown-haiku-ar" t "$a" | head -1)
	rm -rf "$WORK/archcheck" && mkdir -p "$WORK/archcheck"
	(cd "$WORK/archcheck" && "$CROSS_TOOLS/bin/aarch64-unknown-haiku-ar" x "$a" "$member")
	if file "$WORK/archcheck/$member" | grep -q "ARM aarch64"; then
		echo "  OK   $a"
	else
		echo "  FAIL $a is not aarch64" >&2
		exit 1
	fi
done

say "done"
echo "arm64 dependency prefix: $DEPS"
echo
echo "Build with:"
echo "    make ARCH=arm64 MODE=release package"
