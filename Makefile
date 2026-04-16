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
OPENSSL_LIBS   := $(shell $(PKG_CONFIG) --libs   openssl     2>/dev/null || echo -lssl -lcrypto)
LIBEDIT_CFLAGS := $(shell $(PKG_CONFIG) --cflags libedit     2>/dev/null)
LIBEDIT_LIBS   := $(shell $(PKG_CONFIG) --libs   libedit     2>/dev/null || echo -ledit)

CXXFLAGS += $(CURL_CFLAGS) $(JSON_CFLAGS) $(OPENSSL_CFLAGS) $(LIBEDIT_CFLAGS) -pthread
LIBS     := $(CURL_LIBS) $(OPENSSL_LIBS) $(LIBEDIT_LIBS) -pthread

SRCDIR   := src
BIN      := $(BUILDDIR)/claude

SRCS := $(wildcard $(SRCDIR)/*.cpp)
OBJS := $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

PREFIX  ?= /boot/system/non-packaged
BINDIR  ?= $(PREFIX)/bin
MANDIR  ?= $(PREFIX)/documentation/man/man1
DATADIR ?= $(PREFIX)/data/claude-cli

# Haiku vector icon stamped onto the installed binary via
# `addattr … BEOS:ICON`. Optional: if the file is missing or
# we're not on Haiku (no addattr), the stamp step is skipped
# with a dim note. Generated from assets/claude-icon.svg via
# Icon-O-Matic — see the comment block in the SVG.
ICON_HVIF ?= assets/claude-icon.hvif
APP_SIG   ?= application/x-vnd.Microgeni-claude-cli

PKG_NAME    ?= claude_cli
PKG_VERSION ?= 1.3.0
PKG_BUILD   ?= 1
PKG_ARCH    ?= x86_64
PKG_STAGE   := $(BUILDDIR)/pkg
PKG_FILE    := $(BUILDDIR)/$(PKG_NAME)-$(PKG_VERSION)-$(PKG_BUILD)-$(PKG_ARCH).hpkg

.PHONY: all clean install package release

all: $(BIN)

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
	mkdir -p "$(PKG_STAGE)/bin" "$(PKG_STAGE)/documentation/man/man1"
	cp "$(BIN)" "$(PKG_STAGE)/bin/claude"
	cp docs/claude.1 "$(PKG_STAGE)/documentation/man/man1/claude.1"
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
