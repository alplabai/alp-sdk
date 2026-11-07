"""The 'Active SoC capability profile' choice must default per SoC series
(#2952): a plain `west build` for a V2N/V2M CM33 board otherwise resolves to
ALP_SOC_NONE and silently drops the GD32-routed backends."""
import re
from pathlib import Path

CORE = Path(__file__).resolve().parents[2] / "zephyr/kconfigs/core.kconfig"


def test_rzv2n_series_selects_n44_profile() -> None:
    text = CORE.read_text(encoding="utf-8")
    choice = re.search(r'choice\n\tprompt "Active SoC capability profile.*?\nendchoice', text, re.S)
    assert choice
    assert "default ALP_SOC_RENESAS_RZV2N_N44 if SOC_SERIES_RZV2N" in choice.group(0)
