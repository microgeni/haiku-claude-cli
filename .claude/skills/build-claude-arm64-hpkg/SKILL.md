---
name: build-claude-arm64-hpkg
description: Cross-build the arm64 (RPi5) Haiku HPKG for this project
---

# Cross-build the Claude CLI + GUI for Haiku/arm64 (Raspberry Pi 5)

Produces `claude_cli-<version>-1-arm64.hpkg` containing an aarch64
`bin/claude` **and** `apps/Claude`, built on taurus (x86_64 Haiku) with the
cross-tools from a Haiku arm64 build tree. Verified 2026-07-26: installs and
runs on a Pi 5 running Haiku arm64.

## The short version

Once the dependency prefix exists, this is two commands:

```sh
sh ci_scripts/bootstrap_arm64_deps.sh          # only needed once
make ARCH=arm64 MODE=release package
# → build-release-arm64/claude_cli-<version>-1-arm64.hpkg
```

Everything below is *why* it works, for when it stops working.

## Prerequisites

- **arm64 cross-tools**: `/Data/Code/Repos/haiku/generated.arm64/cross-tools-arm64`
  (`aarch64-unknown-haiku-g++`, GCC 13.3.0). Built by Haiku's
  `configure --build-cross-tools arm64`.
- **Haiku source tree**: `/Data/Code/Repos/haiku` — needed for the *private*
  headers (`headers/private/shared`, `headers/private/interface`).
- **arm64 kits**: `generated.arm64/objects/haiku/arm64/release/kits/{shared,tracker}`
  — `libshared.a` and `libtracker.so` live here, NOT in the cross sysroot.
- **haikuports checkout**: `/Data/Code/Repos/haikuports` for the libedit patchset.
- **Host tools**: `cmake`, autotools (`autoreconf`, `libtoolize`), `package`.

## The core constraint: there is no arm64 HaikuPorts repo

```sh
curl -sL https://eu.hpkg.haiku-os.org/haikuports/master/arm64/current/repo
# → []      (x86_64 returns ~2.4 MB, riscv64 ~500 KB)
```

The *only* prebuilt arm64 binaries are Haiku's own bootstrap packages in
`generated.arm64/download/`. That set has curl and OpenSSL but **no libedit
and no yaml-cpp**, which is why the bootstrap script builds those two from
source.

Consequence for packaging: anything not already in the Pi's image must be
linked **statically**, because `pkgman` on the target has no repo to resolve
against. The package therefore requires only `haiku`, `lib:libncursesw` and
`lib:libz` — all present in `haiku-mmc.image`.

What comes from where:

| Dependency | Source |
|---|---|
| OpenSSL 3.4.1 (`libssl.a`, `libcrypto.a`) | already in the cross sysroot |
| ncurses, zlib, libbe, libnetwork, libtracker | cross sysroot / arm64 kits |
| libcurl (`libcurl.a`, static only) | `generated.arm64/download/curl-*-arm64.hpkg` |
| nlohmann/json | header-only — host copy is arch-independent |
| **libedit** | cross-built from source |
| **yaml-cpp** | cross-built from source |

## Three traps that cost real time

1. **`pkg-config` answers for the host.** A plain `make` would happily hand
   x86_64 `-L/packages/...` paths to the aarch64 linker. The Makefile skips
   pkg-config entirely when `ARCH=arm64` and points at the staged prefix.

2. **`findpaths` also answers for the host.** `GUI_PRIVATE_INCLUDES` uses
   `findpaths -e B_FIND_PATH_HEADERS_DIRECTORY private/shared`, which returns
   the host's x86_64 headers. On cross builds it must come from the Haiku
   *source tree* instead. This one fails silently — it compiles.

3. **yaml-cpp 0.8.0 needs `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`.** Its
   `cmake_minimum_required` is too old for current CMake, which hard-errors.

## Rebuilding the dependency prefix from scratch

`ci_scripts/bootstrap_arm64_deps.sh` does all of this; run it directly unless
you are debugging it. It stages into `/Data/Code/cross/haiku-arm64-deps`:

```
include/{curl,editline,nlohmann,yaml-cpp}/  include/histedit.h
lib/{libcurl.a,libedit.a,libyaml-cpp.a}
```

Key details if you need to do it by hand:

- **libedit** 20230828-3.1 from thrysoee.dk, checksum
  `4ee8182b…8814dad`. Apply
  `haikuports/sys-libs/libedit/patches/libedit-20230828_3.1.patchset`
  (Haiku lacks `sys/ttydefaults.h`), then `autoreconf --force --install`
  because the patch touches `configure.ac`. Configure with
  `--host=aarch64-unknown-haiku --build=x86_64-unknown-haiku --disable-shared`,
  `CFLAGS=-I$SYSROOT/develop/headers -I$SYSROOT/develop/headers/ncursesw`,
  `LIBS=-lncursesw`. `make install` fails on the man pages — copy
  `src/.libs/libedit.a`, `src/editline/readline.h` and `src/histedit.h` by hand.
- **yaml-cpp** 0.8.0, checksum `fbe74bbd…3b3a16`. Uses
  `ci_scripts/haiku-arm64-toolchain.cmake`. The HaikuPorts patchset only
  rewires the bundled gtest, so it is NOT needed with tests off.
- **Do not set `CMAKE_SYSROOT`** in the toolchain file. The cross GCC has its
  sysroot compiled in; overriding it breaks the search paths for Haiku's
  non-standard `develop/headers` layout and libstdc++ disappears.

## Verifying before you ship

```sh
# every staged archive must be aarch64 (the bootstrap script asserts this)
package list -i build-release-arm64/claude_cli-*-arm64.hpkg | grep -E "architecture|requires"

# both binaries must be aarch64, and NEEDED must list nothing exotic
mkdir -p /tmp/v && cd /tmp/v && package extract <the>.hpkg
file bin/claude apps/Claude          # → "ELF 64-bit LSB …, ARM aarch64"
aarch64-unknown-haiku-readelf -d bin/claude | grep NEEDED
```

Expected `NEEDED` for the CLI: `libncursesw.so.6`, `libz.so.1`,
`libnetwork.so`, `libbe.so`, `libgcc_s.so.1`, `libroot.so`. The GUI adds
`libtracker.so`. Anything else (a stray `libcurl.so`, `libssl.so`) means a
static link silently became dynamic — the package will fail to install.

**Always re-check the native build too.** The Makefile edits are shared:

```sh
make && make gui     # x86_64 must still build
```

## Installing on the Pi

The bench Pi is not reachable from taurus directly — hop via `asus`, or copy
the file manually. Then:

```sh
pkgman install claude_cli-<version>-1-arm64.hpkg
# or: cp it into /boot/system/packages/
```

## Gotcha: package requires are keyed on ARCH, not BUILD_GUI

`PKG_REQUIRES` must switch on `$(ARCH)`, because static-vs-dynamic linking is
an architecture property. If it is keyed on `BUILD_GUI`, enabling the GUI for
arm64 emits `lib:libcurl` / `lib:libedit` / `lib:libyaml_cpp` into the arm64
package — all unsatisfiable on the target, and the install fails with
confusing dependency errors.

## Known limitations

- **CLI-only fallback**: `make ARCH=arm64 BUILD_GUI=no package` drops
  `apps/Claude` and the `app:Claude` provides. Useful if yaml-cpp breaks.
- Cross-compiling proves it links, not that it runs. `app_server`,
  `BNotification` and the TLS path are only exercised on real hardware.
- No arm64 CI. The Gitea runner builds x86_64 on taurus only.
