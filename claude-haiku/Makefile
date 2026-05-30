# claude-haiku — three build targets:
#   libclaudecore.a : portable C++17, zero BeAPI (API client, agent loop, tools,
#                     command targets, process exec). Links into both shells.
#   claude-gui      : BeAPI front-end, links libcore + libbe + BFS adapters.
#   claude-cli      : terminal front-end, links libcore only. Kept for
#                     SSH/tmux/headless use -- building the GUI does not retire it.
#
# TLS is the riskiest Haiku dependency: prototype the API client against the
# real endpoint before anything else. Swap curl/mbedtls in TLS_LIBS below.

CXX       := g++
CXXFLAGS  := -std=c++17 -Wall -Wextra -O2 -Icore/include -Icli
AR        := ar

TLS_LIBS  := -lcurl          # or: -lmbedtls -lmbedx509 -lmbedcrypto
BE_LIBS   := -lbe            # Haiku Application/Interface Kit

BUILD_DIR := build

CORE_SRC  := core/src/Process.cpp \
             core/src/CommandTarget.cpp \
             core/src/ApiClient.cpp \
             core/src/Tools.cpp \
             core/src/AgentLoop.cpp
CORE_OBJ  := $(patsubst core/src/%.cpp, $(BUILD_DIR)/core/%.o, $(CORE_SRC))
CORE_LIB  := $(BUILD_DIR)/libclaudecore.a

CLI_SRC   := cli/CliRunner.cpp cli/main.cpp
CLI_OBJ   := $(patsubst cli/%.cpp, $(BUILD_DIR)/cli/%.o, $(CLI_SRC))

GUI_SRC   := gui/main.cpp
GUI_OBJ   := $(patsubst gui/%.cpp, $(BUILD_DIR)/gui/%.o, $(GUI_SRC))

.PHONY: all core cli gui clean dirs
all: core cli

# ── core library ─────────────────────────────────────────────────────────────
core: dirs $(CORE_LIB)

$(CORE_LIB): $(CORE_OBJ)
	$(AR) rcs $@ $^

$(BUILD_DIR)/core/%.o: core/src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ── CLI binary ───────────────────────────────────────────────────────────────
cli: core $(CLI_OBJ)
	$(CXX) $(CXXFLAGS) -o $(BUILD_DIR)/claude-cli $(CLI_OBJ) $(CORE_LIB) $(TLS_LIBS)

$(BUILD_DIR)/cli/%.o: cli/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ── GUI binary (BeAPI, Haiku only) ───────────────────────────────────────────
gui: core $(GUI_OBJ)
	$(CXX) $(CXXFLAGS) -o $(BUILD_DIR)/claude-gui $(GUI_OBJ) $(CORE_LIB) $(TLS_LIBS) $(BE_LIBS)

$(BUILD_DIR)/gui/%.o: gui/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ── housekeeping ─────────────────────────────────────────────────────────────
dirs:
	@mkdir -p $(BUILD_DIR)/core $(BUILD_DIR)/cli $(BUILD_DIR)/gui

clean:
	rm -rf $(BUILD_DIR)
