# Dependencies

This file is the single source of truth for everything needed to build and
run the Claude CLI. It is derived from the `pkg-config` calls and link lines
in the `Makefile`; if you change a dependency there, update this file too.

The project is a single C++17 binary. Runtime dependencies are intentionally
minimal: libcurl, OpenSSL, libedit, and nlohmann/json. The optional GUI
front-end (`make gui`, Haiku only) adds yaml-cpp and the BeAPI libraries.


## Build tools (all platforms)

| Tool          | Purpose                          | Required for   |
|---------------|----------------------------------|----------------|
| C++17 compiler| gcc on Haiku, clang on macOS/nix | all builds     |
| GNU make      | build driver (uses `MAKEFLAGS`)  | all builds     |
| pkg-config    | resolves all dependency flags    | all builds     |


## Library dependencies

| Library         | Used by        | Provides                    | pkg-config name |
|-----------------|----------------|-----------------------------|-----------------|
| libcurl         | CLI + GUI      | HTTPS to the Claude API     | `libcurl`       |
| OpenSSL (ssl)   | CLI + GUI      | TLS, OAuth/PKCE crypto      | `openssl`       |
| OpenSSL (crypto)| CLI + GUI      | hashing, base64, PKCE       | `openssl`       |
| libedit         | CLI only       | line editing, history       | `libedit`       |
| nlohmann/json   | CLI + GUI      | JSON (header-only)          | `nlohmann_json` |
| yaml-cpp        | GUI only       | YAML config parsing         | `yaml-cpp`      |
| libbe           | CLI + GUI (Haiku) | BNotification, BeAPI     | — (`-lbe`)      |
| libnetwork      | GUI only (Haiku)  | BeAPI networking         | — (`-lnetwork`) |
| libtracker      | GUI only (Haiku)  | Tracker integration      | — (`-ltracker`) |
| pthread         | CLI + GUI      | threading                   | — (`-pthread`)  |

Notes:
- The CLI build (`make`) links libbe on Haiku for desktop notifications;
  on macOS/nix it is omitted automatically (`HAIKU_LIBS` is empty).
- The GUI build (`make gui`) drops libedit (no terminal line editing) and
  adds yaml-cpp, libnetwork, and libtracker. It is **Haiku-only**.


## Optional tooling

| Tool        | Used by            | Install when you want to…        |
|-------------|--------------------|----------------------------------|
| cppcheck    | `make lint`        | run static analysis              |
| flawfinder  | `make security`    | run the CWE security audit       |
| package     | `make package`     | build an HPKG (Haiku only)       |
| addattr     | install/package    | stamp BEOS:ICON / APP_SIG (Haiku)|


## Installing the dependencies

### Haiku (primary target)

```bash
# Build tools + CLI runtime libraries (devel packages give headers + pkg-config)
pkgman install devel:libcurl devel:libssl nlohmann_json pkgconfig libedit_devel

# Extra libraries needed only for `make gui`
pkgman install yaml_cpp_devel

# Optional: static analysis tooling
pkgman install cppcheck flawfinder
```

`libbe`, `libnetwork`, and `libtracker` ship with the base Haiku system —
no separate install is required. The `package` tool for `make package` is
also part of the base system.

### macOS / nix (prototype only)

The dev shell pins everything via `flake.nix`:

```bash
nix develop            # enters a shell with make, pkg-config, curl,
                       # nlohmann_json, openssl, libedit
nix develop -c make    # build the CLI
```

The GUI target does not build on macOS (it requires the BeAPI), so yaml-cpp
is not part of the nix shell.


## Where these are declared

Dependency information is intentionally kept in sync across these files:

| File              | Scope                                      |
|-------------------|--------------------------------------------|
| `Makefile`        | authoritative — pkg-config + link lines    |
| `.PackageInfo.in` | HPKG runtime `requires` (Haiku package mgr)|
| `flake.nix`       | macOS/nix dev shell packages               |
| `README.md`       | quick-start build instructions             |
| `DEPENDENCIES.md` | this file — the human-readable overview    |
