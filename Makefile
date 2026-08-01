CXXSTD   ?= -std=c++17
WARN     ?= -Wall -Wextra -Wpedantic

# ── Target architecture ─────────────────────────────────────────────────────
# ARCH selects the target. The default is the host architecture, i.e. a
# normal native build. `make ARCH=arm64` cross-compiles for Haiku/aarch64
# (Raspberry Pi 5) using the cross-tools from a Haiku arm64 build tree plus
# a staged dependency prefix built by ci_scripts/bootstrap_arm64_deps.sh.
#
# There is no arm64 HaikuPorts repository, so anything the target image does
# not already ship (libcurl, libedit, OpenSSL, yaml-cpp) is linked statically.
# That leaves only ncurses and zlib as runtime dependencies, both of which
# are part of the Haiku arm64 image.
ARCH ?= $(shell uname -m 2>/dev/null || echo x86_64)

ifeq ($(ARCH),arm64)
    HAIKU_ARM64_TREE ?= /Data/Code/Repos/haiku/generated.arm64
    HAIKU_SRC        ?= /Data/Code/Repos/haiku
    CROSS_TOOLS   ?= $(HAIKU_ARM64_TREE)/cross-tools-arm64
    CROSS_SYSROOT ?= $(CROSS_TOOLS)/sysroot/boot/system
    ARM64_DEPS    ?= /Data/Code/cross/haiku-arm64-deps
    ARM64_KITS    ?= $(HAIKU_ARM64_TREE)/objects/haiku/arm64/release/kits
    CXX           := $(CROSS_TOOLS)/bin/aarch64-unknown-haiku-g++
    BUILD_GUI     ?= yes
else
    CXX      ?= c++
    BUILD_GUI ?= yes
endif

# ── Parallel build ──────────────────────────────────────────────────────────
# Default to one compile job per CPU so a plain `make` uses all cores. The
# user can still override with an explicit `-jN` on the command line (that
# wins because command-line flags take precedence) or pin the count with
# `make JOBS=8`. NPROCS is detected on Haiku/Linux (nproc), macOS/BSD
# (sysctl), then getconf, falling back to 1.
NPROCS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
JOBS   ?= $(NPROCS)
# Only inject -j when the invoking make wasn't already given a job count
# (avoids the "-jN forced in submake" warning and respects an explicit -j).
ifeq ($(filter -j%,$(MAKEFLAGS)),)
    MAKEFLAGS += -j$(JOBS)
endif

# Build mode. `make` is a fast unoptimized-ish dev build; `make release` is
# a separate target below that reinvokes make with MODE=release. Each mode
# gets its own BUILDDIR so switching between them doesn't force a rebuild.
MODE     ?= dev

ifeq ($(MODE),release)
    OPT       ?= -O3 -DNDEBUG
    LTO_FLAGS ?= -flto
    # -Wl,-s strips symbols at link time. Skipped when --no-strip is useful
    # for profiling; set STRIP= to disable.
    STRIP     ?= -Wl,-s
    BUILDDIR  := build-release
else
    OPT       ?= -O2
    LTO_FLAGS :=
    STRIP     :=
    BUILDDIR  := build
endif

# Cross builds get their own tree so a native build isn't invalidated.
ifneq ($(ARCH),$(shell uname -m 2>/dev/null || echo x86_64))
    BUILDDIR := $(BUILDDIR)-$(ARCH)
endif

CXXFLAGS ?= $(CXXSTD) $(WARN) $(OPT) $(LTO_FLAGS)
LDFLAGS  ?= $(LTO_FLAGS) $(STRIP)

PKG_CONFIG ?= pkg-config

ifeq ($(ARCH),arm64)

# Cross build: no target pkg-config exists, so point at the staged prefix
# directly. libcurl, libedit and OpenSSL are the static archives; ncurses,
# zlib and the Haiku kits come from the cross sysroot and stay dynamic.
CURL_CFLAGS    := -I$(ARM64_DEPS)/include
CURL_LIBS      := $(ARM64_DEPS)/lib/libcurl.a
JSON_CFLAGS    := -I$(ARM64_DEPS)/include
OPENSSL_CFLAGS :=
OPENSSL_LIBS   := $(CROSS_SYSROOT)/develop/lib/libssl.a \
                  $(CROSS_SYSROOT)/develop/lib/libcrypto.a
LIBEDIT_CFLAGS := -I$(ARM64_DEPS)/include
LIBEDIT_LIBS   := $(ARM64_DEPS)/lib/libedit.a
HAIKU_LIBS     := -L$(CROSS_SYSROOT)/develop/lib -lncursesw -lz -lnetwork -lbe

else

CURL_CFLAGS    := $(shell $(PKG_CONFIG) --cflags libcurl     2>/dev/null)
CURL_LIBS      := $(shell $(PKG_CONFIG) --libs   libcurl     2>/dev/null || echo -lcurl)
JSON_CFLAGS    := $(shell $(PKG_CONFIG) --cflags nlohmann_json 2>/dev/null)
OPENSSL_CFLAGS := $(shell $(PKG_CONFIG) --cflags openssl     2>/dev/null)
OPENSSL_LIBS   := $(shell $(PKG_CONFIG) --libs   openssl     2>/dev/null || \
                   (ls /boot/system/lib/libssl.so.3 >/dev/null 2>&1 && \
                    echo /boot/system/lib/libssl.so.3 /boot/system/lib/libcrypto.so.3) || \
                   echo -lssl -lcrypto)
LIBEDIT_CFLAGS := $(shell $(PKG_CONFIG) --cflags libedit     2>/dev/null)
LIBEDIT_LIBS   := $(shell $(PKG_CONFIG) --libs   libedit     2>/dev/null || echo -ledit)
HAIKU_LIBS     := $(shell uname -s 2>/dev/null | grep -q Haiku && echo "-lbe" || echo "")

endif

CXXFLAGS += $(CURL_CFLAGS) $(JSON_CFLAGS) $(OPENSSL_CFLAGS) $(LIBEDIT_CFLAGS) -pthread -D_DEFAULT_SOURCE
LIBS     := $(CURL_LIBS) $(OPENSSL_LIBS) $(LIBEDIT_LIBS) -pthread $(HAIKU_LIBS)

SRCDIR   := src
BIN      := $(BUILDDIR)/claude

# GUI-only sources must be excluded from the CLI wildcard so BeAPI
# headers and symbols don't bleed into the terminal build.
GUI_ONLY_SRCS := \
    $(SRCDIR)/gui_sink.cpp          \
    $(SRCDIR)/gui_stubs.cpp         \
    $(SRCDIR)/code_styler.cpp       \
    $(SRCDIR)/md_renderer.cpp       \
    $(SRCDIR)/syntax_highlight.cpp  \
    $(SRCDIR)/tool_bar.cpp          \
    $(SRCDIR)/gui_scale.cpp         \
    $(SRCDIR)/gui_widgets.cpp       \
    $(SRCDIR)/gui_views.cpp         \
    $(SRCDIR)/settings_dialog.cpp   \
    $(SRCDIR)/chat_window.cpp       \
    $(SRCDIR)/chat_window_find.cpp  \
    $(SRCDIR)/chat_window_sessions.cpp \
    $(SRCDIR)/chat_window_prefs.cpp \
    $(SRCDIR)/app_main_gui.cpp

_ALL_SRCS := $(wildcard $(SRCDIR)/*.cpp)
SRCS := $(filter-out $(GUI_ONLY_SRCS), $(_ALL_SRCS))
OBJS := $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

PREFIX  ?= /boot/system/non-packaged
BINDIR  ?= $(PREFIX)/bin
APPSDIR ?= $(PREFIX)/apps
MANDIR  ?= $(PREFIX)/documentation/man/man1
DATADIR ?= $(PREFIX)/data/claude-cli
# The GUI reads syntax-highlight styles/languages from here at runtime
# (see code_styler.cpp FindDefaultTheme / FindLanguagesDir).
GUI_DATADIR ?= $(PREFIX)/data/claude-gui

# Haiku vector icon stamped onto the installed binary via
# `addattr … BEOS:ICON`. Optional: if the file is missing or
# we're not on Haiku (no addattr), the stamp step is skipped
# with a dim note. Generated from assets/claude-icon.svg via
# Icon-O-Matic — see the comment block in the SVG.
ICON_HVIF ?= assets/claude-icon.hvif
APP_SIG   ?= application/x-vnd.Microgeni-claude-cli

PKG_NAME    ?= claude_cli
# Single source of truth for the marketing version: the top-level VERSION
# file. CI overrides PKG_VERSION with the git tag (leading "v" stripped);
# locally it defaults to VERSION so the Makefile, the .hpkg, and the compiled
# binary's config::kVersion can never drift apart.
PKG_VERSION ?= $(strip $(shell cat VERSION 2>/dev/null))
PKG_BUILD   ?= 1
PKG_ARCH    ?= $(ARCH)
PKG_STAGE   := $(BUILDDIR)/pkg

# .PackageInfo token expansion.
#
# The dependency set is a property of the *architecture*, not of the GUI:
# the native build links everything dynamically against HaikuPorts packages,
# while the arm64 cross build statically links libcurl/libedit/OpenSSL/
# yaml-cpp (no arm64 HaikuPorts repo exists) and so needs only ncurses and
# zlib. libtracker/libbe/libnetwork come from the haiku package itself.
#
# ca_root_certificates is required explicitly on arm64: the CA bundle is a
# hard runtime requirement for TLS (libcurl has the path compiled in), and
# the arm64 Haiku image does not ship it. On x86_64 it arrives transitively
# via HaikuPorts' curl, but static linking severs that chain.
ifeq ($(ARCH),arm64)
PKG_REQUIRES := haiku >= r1~beta5\n\tca_root_certificates\n\tlib:libncursesw\n\tlib:libz
else
PKG_REQUIRES := haiku >= r1~beta5\n\tca_root_certificates\n\tlib:libcurl\n\tlib:libcrypto >= 3\n\tlib:libssl >= 3\n\tlib:libedit\n\tlib:libyaml_cpp
endif

# Whether the GUI ships is independent of the above.
ifeq ($(BUILD_GUI),no)
PKG_SUMMARY        := Native Claude client for Haiku OS (CLI)
PKG_PROVIDES_EXTRA :=
else
PKG_SUMMARY        := Native Claude client for Haiku OS (CLI + GUI)
PKG_PROVIDES_EXTRA := \tapp:Claude = $(PKG_VERSION)-$(PKG_BUILD)
endif

# Compile the marketing version into the binary from the same PKG_VERSION,
# so config::kVersion always matches the package that shipped it.
CXXFLAGS += -DCCH_VERSION='"$(PKG_VERSION)"'

PKG_FILE    := $(BUILDDIR)/$(PKG_NAME)-$(PKG_VERSION)-$(PKG_BUILD)-$(PKG_ARCH).hpkg

.PHONY: all gui clean install install-gui package release lint security check test test-unit version-check

all: $(BIN)

# GUI target (Haiku only — links libbe) ────────────────────────────────────
# The GUI reuses all the core logic modules from src/ but substitutes the
# terminal-specific files (main, session, repl, tui, commands, stats,
# terminal_sink, telegram) for the BeAPI front-end files.
#
# The binary is named "Claude" (capitalized, no hyphen) to follow Haiku's
# application naming convention (StyledEdit, Terminal, Tracker). The CLI
# binary stays lowercase "claude". The app signature is an internal MIME
# identifier and is intentionally left stable across the rename.
GUI_BIN     := $(BUILDDIR)/Claude
GUI_APP_SIG ?= application/x-vnd.Microgeni-claude-gui

# Modules shared between CLI and GUI (core logic, no terminal UI).
GUI_CORE_SRCS := \
    $(SRCDIR)/api.cpp         \
    $(SRCDIR)/agents.cpp      \
    $(SRCDIR)/commands.cpp    \
    $(SRCDIR)/config.cpp      \
    $(SRCDIR)/diagnostics.cpp \
    $(SRCDIR)/editor_integration.cpp \
    $(SRCDIR)/hooks.cpp       \
    $(SRCDIR)/history_util.cpp \
    $(SRCDIR)/image_util.cpp  \
    $(SRCDIR)/learn.cpp       \
    $(SRCDIR)/mcp.cpp         \
    $(SRCDIR)/md_text.cpp     \
    $(SRCDIR)/models.cpp      \
    $(SRCDIR)/notify.cpp      \
    $(SRCDIR)/oauth.cpp       \
    $(SRCDIR)/paths.cpp       \
    $(SRCDIR)/skills.cpp      \
    $(SRCDIR)/sse_parser.cpp  \
    $(SRCDIR)/stats.cpp       \
    $(SRCDIR)/transcript_export.cpp \
    $(SRCDIR)/tools.cpp

# GUI-specific front-end files.
GUI_FRONT_SRCS := \
    $(SRCDIR)/tui.cpp               \
    $(SRCDIR)/code_styler.cpp       \
    $(SRCDIR)/md_renderer.cpp       \
    $(SRCDIR)/syntax_highlight.cpp  \
    $(SRCDIR)/gui_stubs.cpp         \
    $(SRCDIR)/gui_sink.cpp          \
    $(SRCDIR)/gui_scale.cpp         \
    $(SRCDIR)/gui_widgets.cpp       \
    $(SRCDIR)/gui_views.cpp         \
    $(SRCDIR)/settings_dialog.cpp   \
    $(SRCDIR)/session_store.cpp     \
    $(SRCDIR)/telegram.cpp          \
    $(SRCDIR)/tool_bar.cpp          \
    $(SRCDIR)/chat_window.cpp       \
    $(SRCDIR)/chat_window_find.cpp  \
    $(SRCDIR)/chat_window_sessions.cpp \
    $(SRCDIR)/chat_window_prefs.cpp \
    $(SRCDIR)/app_main_gui.cpp

GUI_SRCS := $(GUI_CORE_SRCS) $(GUI_FRONT_SRCS)
GUI_OBJS := $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/gui_%.o,$(GUI_SRCS))
GUI_DEPS := $(GUI_OBJS:.o=.d)

ifeq ($(ARCH),arm64)

# Cross build. yaml-cpp comes from the staged prefix as a static archive.
# The private Haiku headers must come from the *source tree* — `findpaths`
# would return the host's x86_64 headers and silently poison the build.
# libshared.a / libtracker.so aren't in the cross sysroot, so point the
# linker at the kits directory of the arm64 build tree.
YAMLCPP_CFLAGS := -I$(ARM64_DEPS)/include
YAMLCPP_LIBS   := $(ARM64_DEPS)/lib/libyaml-cpp.a
GUI_PRIVATE_INCLUDES := -I$(HAIKU_SRC)/headers/private/shared \
                        -I$(HAIKU_SRC)/headers/private/interface
GUI_EXTRA_LIBS := -L$(CROSS_SYSROOT)/develop/lib \
                  -L$(ARM64_KITS)/shared -L$(ARM64_KITS)/tracker \
                  -lncursesw -lz

else

YAMLCPP_CFLAGS := $(shell $(PKG_CONFIG) --cflags yaml-cpp 2>/dev/null)
YAMLCPP_LIBS   := $(shell $(PKG_CONFIG) --libs   yaml-cpp 2>/dev/null || echo -lyaml-cpp)
GUI_PRIVATE_INCLUDES := $(shell findpaths -e B_FIND_PATH_HEADERS_DIRECTORY private/shared 2>/dev/null | sed 's/^/-I/') \
                        $(shell findpaths -e B_FIND_PATH_HEADERS_DIRECTORY private/interface 2>/dev/null | sed 's/^/-I/')
GUI_EXTRA_LIBS :=

endif

# Same compile flags as the CLI + libbe headers + yaml-cpp.
# Private Haiku headers (BPrivate::BToolBar lives in private/shared) are
# added so the Genio-style ToolBar compiles; libshared provides the symbol.
GUI_CXXFLAGS := $(CXXFLAGS) $(YAMLCPP_CFLAGS) $(GUI_PRIVATE_INCLUDES)
GUI_LIBS     := $(CURL_LIBS) $(OPENSSL_LIBS) $(YAMLCPP_LIBS) \
                -pthread -lbe -lshared -lnetwork -ltracker $(GUI_EXTRA_LIBS)

$(BUILDDIR)/gui_%.o: $(SRCDIR)/%.cpp | $(BUILDDIR)
	@mkdir -p $(@D)
	$(CXX) $(GUI_CXXFLAGS) -MMD -MP -MF $(@:.o=.d) -c -o $@ $<

$(GUI_BIN): $(GUI_OBJS) | $(BUILDDIR)
	$(CXX) $(LDFLAGS) -o $@ $^ $(GUI_LIBS)
	@if command -v addattr >/dev/null 2>&1; then \
	    if [ -f "$(ICON_HVIF)" ]; then \
	        echo "  stamping BEOS:ICON on $(GUI_BIN)"; \
	        addattr -t "'VICN'" -f "$(ICON_HVIF)" BEOS:ICON "$@"; \
	    fi; \
	    echo "  stamping BEOS:APP_SIG = $(GUI_APP_SIG)"; \
	    addattr -t mime BEOS:APP_SIG "$(GUI_APP_SIG)" "$@"; \
	fi

-include $(GUI_DEPS)

.PHONY: gui
gui: $(GUI_BIN)

# Install the GUI as a native Haiku application. The binary goes into
# the apps directory (so the Deskbar's app menu and Tracker pick it up),
# and the syntax-highlight style/language data is copied to the runtime
# location code_styler.cpp looks for. Haiku-only — the GUI itself only
# builds there. Run `make gui` first (or it builds via the dependency).
install-gui: $(GUI_BIN)
	install -d "$(DESTDIR)$(APPSDIR)"
	install -m 755 "$(GUI_BIN)" "$(DESTDIR)$(APPSDIR)/Claude"
	@echo "  installing GUI style data to $(GUI_DATADIR)"
	install -d "$(DESTDIR)$(GUI_DATADIR)/styles"
	install -d "$(DESTDIR)$(GUI_DATADIR)/languages"
	@if [ -d assets/data/styles ]; then \
	    install -m 644 assets/data/styles/* "$(DESTDIR)$(GUI_DATADIR)/styles/" 2>/dev/null || true; \
	fi
	@if [ -d assets/data/languages ]; then \
	    install -m 644 assets/data/languages/* "$(DESTDIR)$(GUI_DATADIR)/languages/" 2>/dev/null || true; \
	fi
	@if command -v addattr >/dev/null 2>&1; then \
	    if [ -f "$(ICON_HVIF)" ]; then \
	        echo "  stamping BEOS:ICON on $(APPSDIR)/Claude"; \
	        addattr -t "'VICN'" -f "$(ICON_HVIF)" BEOS:ICON "$(DESTDIR)$(APPSDIR)/Claude"; \
	    fi; \
	    echo "  stamping BEOS:APP_SIG = $(GUI_APP_SIG)"; \
	    addattr -t mime BEOS:APP_SIG "$(GUI_APP_SIG)" "$(DESTDIR)$(APPSDIR)/Claude"; \
	else \
	    echo "  (skipping icon/sig stamp — no addattr)"; \
	fi
	@echo "  installed: $(APPSDIR)/Claude"

# Optimized build in a separate directory so it doesn't invalidate
# incremental dev builds. Reinvokes make with MODE=release.
release:
	@$(MAKE) --no-print-directory MODE=release all

$(BIN): $(OBJS) | $(BUILDDIR)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)
	@if command -v addattr >/dev/null 2>&1; then \
	    if [ -f "$(ICON_HVIF)" ]; then \
	        echo "  stamping BEOS:ICON from $(ICON_HVIF)"; \
	        addattr -t "'VICN'" -f "$(ICON_HVIF)" BEOS:ICON "$@"; \
	    fi; \
	    echo "  stamping BEOS:APP_SIG = $(APP_SIG)"; \
	    addattr -t mime BEOS:APP_SIG "$(APP_SIG)" "$@"; \
	fi

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp | $(BUILDDIR)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(BUILDDIR):
	mkdir -p $@

clean:
	rm -rf build build-release build-arm64 build-release-arm64

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/claude
	install -d $(DESTDIR)$(MANDIR)
	install -m 644 docs/claude.1 $(DESTDIR)$(MANDIR)/claude.1
	@if [ -f "$(ICON_HVIF)" ]; then \
	    echo "  installing icon to $(DATADIR)/icon.hvif"; \
	    install -d "$(DESTDIR)$(DATADIR)"; \
	    install -m 644 "$(ICON_HVIF)" "$(DESTDIR)$(DATADIR)/icon.hvif"; \
	fi
	@if command -v addattr >/dev/null 2>&1; then \
	    if [ -f "$(ICON_HVIF)" ]; then \
	        echo "  stamping BEOS:ICON from $(ICON_HVIF)"; \
	        addattr -t "'VICN'" -f "$(ICON_HVIF)" BEOS:ICON "$(DESTDIR)$(BINDIR)/claude"; \
	    fi; \
	    echo "  stamping BEOS:APP_SIG = $(APP_SIG)"; \
	    addattr -t mime BEOS:APP_SIG "$(APP_SIG)" "$(DESTDIR)$(BINDIR)/claude"; \
	else \
	    echo "  (skipping icon/sig stamp — no addattr)"; \
	fi

# Build a Haiku HPKG. Requires Haiku's `package` tool.
package: $(PKG_FILE)

# One HPKG normally ships both front-ends: the CLI binary (bin/claude) and
# the GUI app (apps/Claude) plus the GUI's syntax-highlight data. On cross
# builds where the GUI can't be built (BUILD_GUI=no) the package is CLI-only.
ifeq ($(BUILD_GUI),no)
PKG_BIN_DEPS := $(BIN)
else
PKG_BIN_DEPS := $(BIN) $(GUI_BIN)
endif

$(PKG_FILE): $(PKG_BIN_DEPS) .PackageInfo.in docs/claude.1 | $(BUILDDIR)
	@command -v package >/dev/null 2>&1 || { \
	    echo "error: 'package' command not found — HPKG build requires Haiku."; \
	    exit 1; \
	}
	rm -rf "$(PKG_STAGE)"
	mkdir -p "$(PKG_STAGE)/bin" \
	         "$(PKG_STAGE)/documentation/man/man1" \
	         "$(PKG_STAGE)/documentation/packages/claude-cli"
	@if [ "$(BUILD_GUI)" != "no" ]; then \
	    mkdir -p "$(PKG_STAGE)/apps" \
	             "$(PKG_STAGE)/data/claude-gui/styles" \
	             "$(PKG_STAGE)/data/claude-gui/languages"; \
	fi
	cp "$(BIN)"      "$(PKG_STAGE)/bin/claude"
	@if [ -f "$(GUI_BIN)" ]; then cp "$(GUI_BIN)" "$(PKG_STAGE)/apps/Claude"; \
	 else echo "  (GUI not built for $(ARCH) — packaging CLI only)"; fi
	cp docs/claude.1  "$(PKG_STAGE)/documentation/man/man1/claude.1"
	cp CHANGELOG.md   "$(PKG_STAGE)/documentation/packages/claude-cli/CHANGELOG.md"
	cp README.md      "$(PKG_STAGE)/documentation/packages/claude-cli/ReadMe.md"
	@# GUI syntax-highlight data (styles + languages).
	@if [ -d assets/data/styles ]; then \
	    cp assets/data/styles/*    "$(PKG_STAGE)/data/claude-gui/styles/"    2>/dev/null || true; \
	fi
	@if [ -d assets/data/languages ]; then \
	    cp assets/data/languages/* "$(PKG_STAGE)/data/claude-gui/languages/" 2>/dev/null || true; \
	fi
	@if [ -f "$(ICON_HVIF)" ]; then \
	    echo "  staging icon to data/claude-cli/icon.hvif"; \
	    mkdir -p "$(PKG_STAGE)/data/claude-cli"; \
	    cp "$(ICON_HVIF)" "$(PKG_STAGE)/data/claude-cli/icon.hvif"; \
	fi
	@if command -v addattr >/dev/null 2>&1; then \
	    if [ -f "$(ICON_HVIF)" ]; then \
	        echo "  stamping BEOS:ICON onto staged binaries"; \
	        addattr -t "'VICN'" -f "$(ICON_HVIF)" BEOS:ICON "$(PKG_STAGE)/bin/claude"; \
	        [ -f "$(PKG_STAGE)/apps/Claude" ] && \
	            addattr -t "'VICN'" -f "$(ICON_HVIF)" BEOS:ICON "$(PKG_STAGE)/apps/Claude" || true; \
	    fi; \
	    echo "  stamping BEOS:APP_SIG (cli=$(APP_SIG), gui=$(GUI_APP_SIG))"; \
	    addattr -t mime BEOS:APP_SIG "$(APP_SIG)"     "$(PKG_STAGE)/bin/claude"; \
	    [ -f "$(PKG_STAGE)/apps/Claude" ] && \
	        addattr -t mime BEOS:APP_SIG "$(GUI_APP_SIG)" "$(PKG_STAGE)/apps/Claude" || true; \
	fi
	sed -e 's/@VERSION@/$(PKG_VERSION)/g' \
	    -e 's/@BUILD@/$(PKG_BUILD)/g' \
	    -e 's/@ARCH@/$(PKG_ARCH)/g' \
	    -e 's|@SUMMARY@|$(PKG_SUMMARY)|g' \
	    -e 's|@PROVIDES_EXTRA@|$(PKG_PROVIDES_EXTRA)|g' \
	    -e 's|@REQUIRES@|\t$(PKG_REQUIRES)|g' \
	    .PackageInfo.in > "$(PKG_STAGE)/.PackageInfo"
	rm -f "$(PKG_FILE)"
	package create -C "$(PKG_STAGE)" "$(PKG_FILE)"
	@echo
	@echo "Created: $(PKG_FILE)"
	@ls -l "$(PKG_FILE)"

-include $(DEPS)

# ---------------------------------------------------------------------------
# Static analysis
# ---------------------------------------------------------------------------

# cppcheck — fast, low-noise static analysis.
# Checks warning + performance + portability categories.
# Suppressed:
#   missingIncludeSystem  — system headers not available to cppcheck
#   unusedFunction        — public API called across translation units
#   knownConditionTrueFalse — defensive guards cppcheck proves redundant
#   variableScope         — lambda-capture variables flagged incorrectly
#   useStlAlgorithm       — style preference, not a bug
#   unmatchedSuppression  — fired when a suppression is never triggered
#                           (happens on non-Haiku builds missing BFS code)
# ── Tests ───────────────────────────────────────────────────────────────────
# `make test-unit` compiles and runs the doctest-based unit tests in
# tests/unit/. These cover the pure-logic modules (no BeAPI, no network) so
# they build and run on every target, including the macOS/nix dev shell.
#
# `make test` runs the unit tests and then the functional suite in
# ci_scripts/test.sh against the freshly built CLI binary. This is the target
# CLAUDE.md and the release checklist refer to.
UNIT_BUILDDIR := $(BUILDDIR)/unit
UNIT_CXXFLAGS := $(CXXSTD) $(WARN) -O1 -Itests/unit

$(UNIT_BUILDDIR):
	mkdir -p $(UNIT_BUILDDIR)

# md_text unit test — links only the pure md_text.cpp translation unit.
$(UNIT_BUILDDIR)/md_text_test: tests/unit/md_text_test.cpp src/md_text.cpp \
        tests/unit/doctest.h | $(UNIT_BUILDDIR)
	$(CXX) $(UNIT_CXXFLAGS) -o $@ tests/unit/md_text_test.cpp src/md_text.cpp

# sse_parser unit test — links the pure sse_parser.cpp translation unit.
# nlohmann/json headers come from pkg-config (JSON_CFLAGS).
$(UNIT_BUILDDIR)/sse_parser_test: tests/unit/sse_parser_test.cpp src/sse_parser.cpp \
        tests/unit/doctest.h | $(UNIT_BUILDDIR)
	$(CXX) $(UNIT_CXXFLAGS) $(JSON_CFLAGS) -o $@ \
	    tests/unit/sse_parser_test.cpp src/sse_parser.cpp

# history_util unit test — links the pure history_util.cpp translation unit.
$(UNIT_BUILDDIR)/history_util_test: tests/unit/history_util_test.cpp src/history_util.cpp \
        tests/unit/doctest.h | $(UNIT_BUILDDIR)
	$(CXX) $(UNIT_CXXFLAGS) $(JSON_CFLAGS) -o $@ \
	    tests/unit/history_util_test.cpp src/history_util.cpp

# transcript_export unit test — links the pure transcript_export.cpp TU.
$(UNIT_BUILDDIR)/transcript_export_test: tests/unit/transcript_export_test.cpp \
        src/transcript_export.cpp tests/unit/doctest.h | $(UNIT_BUILDDIR)
	$(CXX) $(UNIT_CXXFLAGS) $(JSON_CFLAGS) -o $@ \
	    tests/unit/transcript_export_test.cpp src/transcript_export.cpp

# image_util unit test — links the pure image_util.cpp TU.
$(UNIT_BUILDDIR)/image_util_test: tests/unit/image_util_test.cpp \
        src/image_util.cpp tests/unit/doctest.h | $(UNIT_BUILDDIR)
	$(CXX) $(UNIT_CXXFLAGS) $(JSON_CFLAGS) -o $@ \
	    tests/unit/image_util_test.cpp src/image_util.cpp

# markov unit test — links the pure markov.cpp TU (no BeAPI, no network).
$(UNIT_BUILDDIR)/markov_test: tests/unit/markov_test.cpp \
        src/markov.cpp tests/unit/doctest.h | $(UNIT_BUILDDIR)
	$(CXX) $(UNIT_CXXFLAGS) $(JSON_CFLAGS) -o $@ \
	    tests/unit/markov_test.cpp src/markov.cpp

# workflow unit test — workflow.cpp pulls in markov.cpp for the model and
# paths.cpp for the per-repo state directory.
$(UNIT_BUILDDIR)/workflow_test: tests/unit/workflow_test.cpp \
        src/workflow.cpp src/markov.cpp src/paths.cpp \
        tests/unit/doctest.h | $(UNIT_BUILDDIR)
	$(CXX) $(UNIT_CXXFLAGS) $(JSON_CFLAGS) -o $@ \
	    tests/unit/workflow_test.cpp src/workflow.cpp src/markov.cpp \
	    src/paths.cpp

UNIT_BINS := $(UNIT_BUILDDIR)/md_text_test $(UNIT_BUILDDIR)/sse_parser_test \
             $(UNIT_BUILDDIR)/history_util_test \
             $(UNIT_BUILDDIR)/transcript_export_test \
             $(UNIT_BUILDDIR)/image_util_test \
             $(UNIT_BUILDDIR)/markov_test \
             $(UNIT_BUILDDIR)/workflow_test

test-unit: $(UNIT_BINS)
	@echo "=== unit tests ==="
	@fail=0; for t in $(UNIT_BINS); do \
	    echo "--- $$t ---"; \
	    "$$t" || fail=1; \
	done; \
	if [ "$$fail" != "0" ]; then echo "unit tests FAILED"; exit 1; fi; \
	echo "unit tests passed."

test: test-unit $(BIN)
	@echo "=== functional tests ==="
	bash ci_scripts/test.sh
	@echo "=== all tests passed ==="

lint:
	@echo "=== cppcheck: static analysis ==="
	@command -v cppcheck >/dev/null 2>&1 || { \
	    echo "cppcheck not found — install cppcheck to run lint"; exit 1; }
	cppcheck --enable=warning,performance,portability \
	    --error-exitcode=1 \
	    --suppress=missingIncludeSystem \
	    --suppress=unusedFunction \
	    --suppress=knownConditionTrueFalse \
	    --suppress=variableScope \
	    --suppress=useStlAlgorithm \
	    --suppress=unmatchedSuppression \
	    --suppress=normalCheckLevelMaxBranches \
	    --quiet \
	    $(SRCDIR)/
	@echo "cppcheck passed."

# flawfinder — security audit for dangerous function patterns (CWE).
# Minimum level 3 surfaces real concerns only:
#   level 5  TOCTOU races (chmod/open)   — fixed at source
#   level 4  shell execution             — annotated // flawfinder: ignore
#            where intentional (Bash tool, hooks, MCP, editor launch)
#   level 3  format-string / crypto APIs — reviewed
#   level 2  generic buffer / char       — mostly false positives; use
#            `make security-full` (--minlevel=2) for a complete scan.
security:
	@echo "=== flawfinder: security audit (level 3+) ==="
	@command -v flawfinder >/dev/null 2>&1 || { \
	    echo "flawfinder not found — install flawfinder to run security audit"; exit 1; }
	flawfinder --minlevel=3 --quiet $(SRCDIR)/
	@echo "flawfinder complete."

# Full security scan including level-2 buffer/char warnings.
security-full:
	@echo "=== flawfinder: full security audit (level 2+) ==="
	flawfinder --minlevel=2 --quiet $(SRCDIR)/

# version-check — fail if VERSION has no matching CHANGELOG.md section.
# Guards against shipping a binary whose version has no release notes. A
# '-dev'/pre-release suffix in VERSION is exempt. Cheap; wired into check.
version-check:
	@bash ci_scripts/version_check.sh

# check — run all analysis tools in sequence (useful for CI and pre-release).
check: version-check lint security
	@echo "=== all checks passed ==="

