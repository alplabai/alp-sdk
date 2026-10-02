# SPDX-License-Identifier: Apache-2.0
"""ConsoleSwdProbe that reads the GD32 images and SWD tools from the SD payload store.

Without a network the stock probe base64-pushes every GD32 image through the 115200-baud
console (about 13 min per unit). With the images in the store the board copies them
locally; a miss or a hash mismatch falls back to that push and caches the file."""

from __future__ import annotations

from pathlib import Path

from provision.console_target import REMOTE_DIR, ConsoleSwdProbe
from provision.payload_store import PayloadStore, stage


class StoreSwdProbe(ConsoleSwdProbe):
    def __init__(self, target, tools_dir: Path, tools: tuple[str, ...], store: PayloadStore) -> None:
        super().__init__(target, tools_dir, tools)
        self.store = store

    def _py(self, args: str, timeout: float = 600.0, long_running: bool = False) -> str:
        if not self._pushed:
            self.t.run(f"mkdir -p {REMOTE_DIR}")
            for f in self.tools:
                stage(self.t, self.store, self.tools_dir / f, f"{REMOTE_DIR}/{f}")
            self._pushed = True
        return super()._py(args, timeout, long_running)

    def loadbin(self, path: Path, addr: int) -> None:
        stage(self.t, self.store, Path(path), f"{REMOTE_DIR}/img.bin")
        self._py(f"gd32_swd_flash.py write {addr:#x} {REMOTE_DIR}/img.bin", long_running=True)
