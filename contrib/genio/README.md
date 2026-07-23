# Genio IDE integration

The Claude desktop app (`Claude`) integrates with the
[Genio](https://github.com/Genio-The-Haiku-IDE/Genio) IDE: launch Claude
from Genio's **Tools ▸ Claude** menu and every file Claude writes or edits
is opened (or refreshed) in the live Genio editor, with the cursor jumped to
the edited line.

## How it works

Genio's extension system runs any executable dropped into its extensions
directory and passes the active editing context on the command line. When
the user picks **Tools ▸ Claude**, Genio launches the extension with:

```
--project-dir <path>     active project root
--file        <path>     focused editor's file
--line        <n>        caret line
--selection   <s:c-s:c>  selection range
--selection-file <path>  temp file with the highlighted text
```

The Claude GUI parses these directly (see `app_main_gui.cpp`,
`ArgvReceived`). The presence of `--project-dir` / `--file` is the
**provenance signal**: it tells Claude it was launched from Genio, which
activates the round-trip. After Claude's `Write` or `Edit` tool succeeds,
`editor::NotifyFileChanged()` sends a `B_REFS_RECEIVED` message (with
`be:line`) to Genio's application signature (`application/x-vnd.Genio`),
which Genio opens at the edited line.

For a directly-launched GUI or the CLI, none of those flags are present, so
the integration stays completely inert.

The command-line flags Genio passes are one half of a general-purpose IPC:
the Claude GUI accepts the same request as an `'ASKP'` `BMessage` from any
application. If you are integrating another editor or tool, see
[`docs/IPC.md`](../../docs/IPC.md) for the full protocol.

## Installation

`Claude-C` is the Genio extension wrapper. The trailing `-C` makes Genio
label it **"Claude"** with the **Alt+C** shortcut.

```sh
cp contrib/genio/Claude-C ~/config/settings/Genio/extensions/Claude-C
chmod +x ~/config/settings/Genio/extensions/Claude-C
```

Restart Genio (or reopen the Tools menu) and **Tools ▸ Claude** appears.

The wrapper defaults to the system install at
`/boot/system/non-packaged/apps/Claude`. If you run a development build,
edit `CLAUDE_BIN` near the top of the script to point at your
`build/Claude`.

### Important

The wrapper **forwards Genio's arguments verbatim**. Do not rewrite
`--project-dir` to `--working-dir` — doing so strips the provenance signal
and silently disables the round-trip (the GUI still opens, but edits no
longer reflect back into Genio).

### Optional: scope the menu item

You can gate when **Claude** appears in the Tools menu using BFS attributes
on the extension file:

```sh
addattr genio:scope      "editor,project"   ~/config/settings/Genio/extensions/Claude-C
addattr genio:file_types "cpp,h,c,cxx,hpp"  ~/config/settings/Genio/extensions/Claude-C
```

## Verifying

With logging enabled, a launch from Genio records (in
`~/config/settings/claude-cli/logs/`):

```
gui ReadyToRun fromGenio=yes argv=[... --project-dir ... --file ... --line ...]
```

and each subsequent edit records:

```
NotifyFileChanged: Edit -> opening in Genio: <path> line=<n>
OpenInGenio: sent ref to running Genio for '<path>' status=No error
```

If you instead see `fromGenio=no` with `--working-dir` in the argv, the
wrapper is rewriting arguments — reinstall the verbatim-forwarding version
from this directory.
