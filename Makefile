CXX      ?= c++
CXXSTD   ?= -std=c++17
WARN     ?= -Wall -Wextra -Wpedantic
OPT      ?= -O2
CXXFLAGS ?= $(CXXSTD) $(WARN) $(OPT)
LDFLAGS  ?=

PKG_CONFIG ?= pkg-config
CURL_CFLAGS    := $(shell $(PKG_CONFIG) --cflags libcurl     2>/dev/null)
CURL_LIBS      := $(shell $(PKG_CONFIG) --libs   libcurl     2>/dev/null || echo -lcurl)
JSON_CFLAGS    := $(shell $(PKG_CONFIG) --cflags nlohmann_json 2>/dev/null)
OPENSSL_CFLAGS := $(shell $(PKG_CONFIG) --cflags openssl     2>/dev/null)
OPENSSL_LIBS   := $(shell $(PKG_CONFIG) --libs   openssl     2>/dev/null || echo -lssl -lcrypto)

CXXFLAGS += $(CURL_CFLAGS) $(JSON_CFLAGS) $(OPENSSL_CFLAGS)
LIBS     := $(CURL_LIBS) $(OPENSSL_LIBS)

SRCDIR   := src
BUILDDIR := build
BIN      := $(BUILDDIR)/claude

SRCS := $(wildcard $(SRCDIR)/*.cpp)
OBJS := $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

PREFIX  ?= /boot/system/non-packaged
BINDIR  ?= $(PREFIX)/bin

PKG_NAME    ?= claude_cli
PKG_VERSION ?= 0.1.0
PKG_BUILD   ?= 1
PKG_ARCH    ?= x86_64
PKG_STAGE   := $(BUILDDIR)/pkg
PKG_FILE    := $(BUILDDIR)/$(PKG_NAME)-$(PKG_VERSION)-$(PKG_BUILD)-$(PKG_ARCH).hpkg

.PHONY: all clean install package

all: $(BIN)

$(BIN): $(OBJS) | $(BUILDDIR)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(BUILDDIR):
	mkdir -p $@

clean:
	rm -rf $(BUILDDIR)

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/claude

# Build a Haiku HPKG. Requires Haiku's `package` tool.
package: $(PKG_FILE)

$(PKG_FILE): $(BIN) .PackageInfo.in | $(BUILDDIR)
	@command -v package >/dev/null 2>&1 || { \
	    echo "error: 'package' command not found — HPKG build requires Haiku."; \
	    exit 1; \
	}
	rm -rf "$(PKG_STAGE)"
	mkdir -p "$(PKG_STAGE)/bin"
	cp "$(BIN)" "$(PKG_STAGE)/bin/claude"
	sed -e 's/@VERSION@/$(PKG_VERSION)/g' \
	    -e 's/@BUILD@/$(PKG_BUILD)/g' \
	    .PackageInfo.in > "$(PKG_STAGE)/.PackageInfo"
	rm -f "$(PKG_FILE)"
	package create -C "$(PKG_STAGE)" "$(PKG_FILE)"
	@echo
	@echo "Created: $(PKG_FILE)"
	@ls -l "$(PKG_FILE)"

-include $(DEPS)
