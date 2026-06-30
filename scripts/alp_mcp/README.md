# alp-mcp -- the Alp SDK Model Context Protocol server

`alp-mcp` is a small [Model Context Protocol](https://modelcontextprotocol.io)
server that exposes the Alp SDK's machine-readable surface to any MCP-capable
agent (Claude Desktop, Claude Code, or any other MCP client). An agent can then
answer questions like *"which SoMs have PCIe?"*, *"what does the `<alp/adc.h>`
API look like?"* or *"validate this board.yaml"* directly against the SDK
instead of guessing.

## What it is (and why it lives here)

The server is a **thin consumer** of the SDK's own single sources of truth:

- The **DATA** tools read `metadata/catalog.json` -- the generated,
  CI-verified map of every SoM, example, emit mode, portable-API header and
  gate. No SDK runtime is required for these.
- The **LIVE** tools shell the SDK's own CLIs (`scripts/validate_board_yaml.py`
  and `python -m alp_orchestrate`) and return their real output.

Because the live tools call the SDK runtime and the data tool reads the SDK's
own catalog, the server is **co-located inside `alp-sdk` by design** -- it
always moves in lockstep with the SDK it describes, and it ships as a console
script (`alp-mcp`) rather than a separate repo.

## Tools

### Data tools (read the catalog)

| Tool | What it answers |
| ---- | --------------- |
| `list_soms()` | Every SoM SKU with family, silicon, SoC part and core topology. |
| `som_info(sku)` | The full catalog record for one SoM (peripherals, capabilities, topology). |
| `peripheral_support(sku?, peripheral?)` | Which SoMs expose a peripheral, which peripherals a SoM has, or a single yes/no. |
| `list_examples(category?, peripheral?)` | The example apps, filterable by category or peripheral keyword. |
| `list_emit_modes()` | The artefacts the SDK can `--emit` from a board.yaml. |
| `portable_api(header?)` | The `<alp/*>` headers and their `alp_*` functions. |
| `list_gates()` | The CI gate scripts and what each enforces. |

### Live tools (shell the SDK runtime)

| Tool | What it does |
| ---- | ------------ |
| `validate_board_yaml(path_or_content)` | Runs the board.yaml validator on a file path or inline YAML and returns the diagnostics. |
| `emit(board_yaml_path, mode)` | Runs `python -m alp_orchestrate --emit <mode> --input <path>` and returns the generated artefact. |

Live tools never silently succeed: a missing tool or input file returns a
clear `{"ok": false, "error": ...}` object.

## Install

The MCP runtime is an optional extra. From an Alp SDK checkout:

```sh
pip install -e '.[mcp]'
```

This installs the `mcp` package and the `alp-mcp` console script.

## Wire it into an MCP client

Add a server entry to your MCP client's config. For a checkout-based install,
point it at the `alp-mcp` console script:

```json
{
  "mcpServers": {
    "alp-sdk": {
      "command": "alp-mcp"
    }
  }
}
```

If the SDK is published, you can run it without a manual install using `uvx`:

```json
{
  "mcpServers": {
    "alp-sdk": {
      "command": "uvx",
      "args": ["--from", "alp-sdk-cli[mcp]", "alp-mcp"]
    }
  }
}
```

### Pointing at a specific checkout

The server locates the catalog by walking up from its own install location.
If you run it from outside an SDK checkout, set `ALP_SDK_ROOT` to the SDK root
(the directory that contains `metadata/catalog.json`):

```json
{
  "mcpServers": {
    "alp-sdk": {
      "command": "alp-mcp",
      "env": { "ALP_SDK_ROOT": "/path/to/your/alp-sdk" }
    }
  }
}
```

## Transport

`alp-mcp` speaks MCP over **stdio** -- the standard transport for local MCP
clients. There is nothing to expose on the network.
