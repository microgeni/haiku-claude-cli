# claude-haiku — three build targets:
#   libclaudecore.a : portable C++17, zero BeAPI (API client, agent loop, tools,
#                     command targets, process exec). Links into both shells.
#   claude-gui      : BeAPI front-end, links libcore + libbe + BFS adapters.
#   claude-cli      : termios/TUI front-end, links libcore only. Kept for
#                     SSH/tmux/headless use -- building the GUI does not retire it.
#
# TLS is the riskiest Haiku dependency: prototype the API client against the
# real endpoint before anything else. Swap curl/mbedtls in TLS_LIBS below.

CXX       := g++
CXXFLAGS  := -std=c++17 -Wall -Wextra -O2 -Icore/include
AR        := ar

TLS_LIBS  := -lcurl          # or: -lmbedtls -lmbedx509 -lmbedcrypto
BE_LIBS   := -lbe            # Haiku Application/Interface Kit

CORE_SRC  := $(wildcard core/src/*.cpp)
CORE_OBJ  := $(CORE_SRC:.cpp=.o)
CORE_LIB  := libclaudecore.a

GUI_SRC   := $(wildcard gui/*.cpp)
GUI_OBJ   := $(GUI_SRC:.cpp=.o)

CLI_SRC   := $(wildcard cli/*.cpp)
CLI_OBJ   := $(CLI_SRC:.cpp=.o)

.PHONY: all core gui cli clean
all: core gui cli

core: $(CORE_LIB)
$(CORE_LIB): $(CORE_OBJ)
	$(AR) rcs $@ $^

gui: $(GUI_OBJ) $(CORE_LIB)
	$(CXX) $(CXXFLAGS) -o claude-gui $(GUI_OBJ) $(CORE_LIB) $(TLS_LIBS) $(BE_LIBS)

cli: $(CLI_OBJ) $(CORE_LIB)
	$(CXX) $(CXXFLAGS) -o claude-cli $(CLI_OBJ) $(CORE_LIB) $(TLS_LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(CORE_OBJ) $(GUI_OBJ) $(CLI_OBJ) $(CORE_LIB) claude-gui claude-cli
