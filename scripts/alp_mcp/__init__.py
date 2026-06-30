# SPDX-License-Identifier: Apache-2.0
"""alp-mcp -- a customer-facing Model Context Protocol (MCP) server for the Alp SDK.

This package is a *thin consumer* of the SDK's own single-source surfaces:

  * the machine-readable SDK catalog (``metadata/catalog.json``), and
  * the SDK runtime CLIs (the board.yaml validator + the emit orchestrator).

It is co-located inside ``alp-sdk`` on purpose: the live tools shell out to the
SDK's own scripts, so the server and the SDK always move in lockstep.

See ``scripts/alp_mcp/README.md`` for customer wiring instructions.
"""

from alp_mcp.server import TOOL_FUNCTIONS, build_server, main

__all__ = ["TOOL_FUNCTIONS", "build_server", "main"]
