CXX      ?= c++
CXXSTD   ?= -std=c++17
WARN     ?= -Wall -Wextra -Wpedantic

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

CXXFLAGS ?= $(CXXSTD) $(WARN) $(OPT) $(LTO_FLAGS)
LDFLAGS  ?= $(LTO_FLAGS) $(STRIP)

PKG_CONFIG ?= pkg-config
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

CXXFLAGS += $(CURL_CFLAGS) $(JSON_CFLAGS) $(OPENSSL_CFLAGS) $(LIBEDIT_CFLAGS) -pthread -D_DEFAULT_SOURCE
HAIKU_LIBS := $(shell uname -s 2>/dev/null | grep -q Haiku && echo "-lbe" || echo "")
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
    $(SRCDIR)/chat_window.cpp       \
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
PKG_VERSION ?= 1.8.1
PKG_BUILD   ?= 1
PKG_ARCH    ?= x86_64
PKG_STAGE   := $(BUILDDIR)/pkg
PKG_FILE    := $(BUILDDIR)/$(PKG_NAME)-$(PKG_VERSION)-$(PKG_BUILD)-$(PKG_ARCH).hpkg

.PHONY: all gui clean install install-gui package release lint security check

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
    $(SRCDIR)/commands.cpp    \
    $(SRCDIR)/config.cpp      \
    $(SRCDIR)/hooks.cpp       \
    $(SRCDIR)/mcp.cpp         \
    $(SRCDIR)/models.cpp      \
    $(SRCDIR)/notify.cpp      \
    $(SRCDIR)/oauth.cpp       \
    $(SRCDIR)/paths.cpp       \
    $(SRCDIR)/stats.cpp       \
    $(SRCDIR)/tools.cpp

# GUI-specific front-end files.
GUI_FRONT_SRCS := \
    $(SRCDIR)/tui.cpp               \
    $(SRCDIR)/code_styler.cpp       \
    $(SRCDIR)/md_renderer.cpp       \
    $(SRCDIR)/syntax_highlight.cpp  \
    $(SRCDIR)/gui_stubs.cpp         \
    $(SRCDIR)/gui_sink.cpp          \
    $(SRCDIR)/session_store.cpp     \
    $(SRCDIR)/chat_window.cpp       \
    $(SRCDIR)/app_main_gui.cpp

GUI_SRCS := $(GUI_CORE_SRCS) $(GUI_FRONT_SRCS)
GUI_OBJS := $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/gui_%.o,$(GUI_SRCS))
GUI_DEPS := $(GUI_OBJS:.o=.d)

YAMLCPP_CFLAGS := $(shell $(PKG_CONFIG) --cflags yaml-cpp 2>/dev/null)
YAMLCPP_LIBS   := $(shell $(PKG_CONFIG) --libs   yaml-cpp 2>/dev/null || echo -lyaml-cpp)

# Same compile flags as the CLI + libbe headers + yaml-cpp.
GUI_CXXFLAGS := $(CXXFLAGS) $(YAMLCPP_CFLAGS)
GUI_LIBS     := $(CURL_LIBS) $(OPENSSL_LIBS) $(YAMLCPP_LIBS) \
                -pthread -lbe -lnetwork -ltracker

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
	rm -rf build build-release

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

$(PKG_FILE): $(BIN) .PackageInfo.in docs/claude.1 | $(BUILDDIR)
	@command -v package >/dev/null 2>&1 || { \
	    echo "error: 'package' command not found — HPKG build requires Haiku."; \
	    exit 1; \
	}
	rm -rf "$(PKG_STAGE)"
	mkdir -p "$(PKG_STAGE)/bin" \
	         "$(PKG_STAGE)/documentation/man/man1" \
	         "$(PKG_STAGE)/documentation/packages/claude-cli"
	cp "$(BIN)" "$(PKG_STAGE)/bin/claude"
	cp docs/claude.1  "$(PKG_STAGE)/documentation/man/man1/claude.1"
	cp CHANGELOG.md   "$(PKG_STAGE)/documentation/packages/claude-cli/CHANGELOG.md"
	cp README.md      "$(PKG_STAGE)/documentation/packages/claude-cli/ReadMe.md"
	@if [ -f "$(ICON_HVIF)" ]; then \
	    echo "  staging icon to data/claude-cli/icon.hvif"; \
	    mkdir -p "$(PKG_STAGE)/data/claude-cli"; \
	    cp "$(ICON_HVIF)" "$(PKG_STAGE)/data/claude-cli/icon.hvif"; \
	fi
	@if command -v addattr >/dev/null 2>&1; then \
	    if [ -f "$(ICON_HVIF)" ]; then \
	        echo "  stamping BEOS:ICON from $(ICON_HVIF) onto staged binary"; \
	        addattr -t "'VICN'" -f "$(ICON_HVIF)" BEOS:ICON "$(PKG_STAGE)/bin/claude"; \
	    fi; \
	    echo "  stamping BEOS:APP_SIG = $(APP_SIG) onto staged binary"; \
	    addattr -t mime BEOS:APP_SIG "$(APP_SIG)" "$(PKG_STAGE)/bin/claude"; \
	fi
	sed -e 's/@VERSION@/$(PKG_VERSION)/g' \
	    -e 's/@BUILD@/$(PKG_BUILD)/g' \
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

# check — run all analysis tools in sequence (useful for CI and pre-release).
check: lint security
	@echo "=== all checks passed ==="

