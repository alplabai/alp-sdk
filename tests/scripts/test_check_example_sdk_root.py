# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/check_example_sdk_root.py (issue #1390).

The gate pins one invariant: an example's CMakeLists.txt names
scripts/alp_project.py under ${ALP_SDK_ROOT}, never through a bare relative
hop that only resolves while the example sits inside the SDK tree.
"""
from pathlib import Path

from check_example_sdk_root import find_problems

# The shape every example must use -- verbatim from
# examples/peripheral-io/pwm-led-fade/CMakeLists.txt.
GOOD = """\
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)

if(DEFINED ENV{ALP_SDK_ROOT})
    set(ALP_SDK_ROOT $ENV{ALP_SDK_ROOT})
else()
    get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

execute_process(
    COMMAND ${Python3_EXECUTABLE} ${ALP_SDK_ROOT}/scripts/alp_project.py
            --input ${CMAKE_CURRENT_SOURCE_DIR}/board.yaml
            --emit zephyr-conf --core m55_hp
            --output ${_alp_generated}
    RESULT_VARIABLE _alp_rv
    OUTPUT_VARIABLE _alp_stdout
    ERROR_VARIABLE  _alp_stderr
)
"""

# The defect issue #1390 filed, verbatim from the pre-fix
# examples/peripheral-io/drone-autopilot/CMakeLists.txt.
BAD = """\
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

execute_process(
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/alp_project.py
            --input  ${CMAKE_CURRENT_SOURCE_DIR}/board.yaml
            --output ${CMAKE_CURRENT_BINARY_DIR}/alp.conf
            --emit zephyr-conf --core m55_hp
    RESULT_VARIABLE rc
)
"""


def _write(root: Path, rel: str, text: str) -> None:
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8")


def test_empty_tree_passes(tmp_path):
    """No examples/ directory at all -> no problems."""
    assert find_problems(tmp_path) == []


def test_sdk_root_shape_passes(tmp_path):
    """The pwm-led-fade shape is what the gate wants -> no problems."""
    _write(tmp_path, "examples/peripheral-io/pwm-led-fade/CMakeLists.txt", GOOD)
    assert find_problems(tmp_path) == []


def test_bare_relative_hop_fails(tmp_path):
    """The exact #1390 defect: `${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/
    alp_project.py`. Flagged, naming the file, the line and the offending
    token."""
    _write(tmp_path,
           "examples/peripheral-io/drone-autopilot/CMakeLists.txt", BAD)
    problems = find_problems(tmp_path)
    assert len(problems) == 1
    (problem,) = problems
    assert "examples/peripheral-io/drone-autopilot/CMakeLists.txt:8" in problem
    assert "${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/alp_project.py" in problem
    assert "${ALP_SDK_ROOT}/scripts/alp_project.py" in problem
    assert "#1390" in problem


def test_prose_mentioning_the_loader_is_not_an_invocation(tmp_path):
    """A comment saying the example does NOT use the loader must not trip the
    gate -- 27 aen-*/v2n-* examples carry exactly this line, and a naive
    substring match would fail every one of them."""
    _write(tmp_path, "examples/aen/aen-adc-regcheck/CMakeLists.txt", """\
# SPDX-License-Identifier: Apache-2.0
# Plain prj.conf app (no alp_project.py board.yaml emit): the driver Kconfig
# is sourced under CONFIG_ALP_SDK, which prj.conf sets.
# A stale note about ${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/alp_project.py
cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
""")
    assert find_problems(tmp_path) == []


def test_relative_input_path_stays_legal(tmp_path):
    """`..` elsewhere on the command is NOT the defect. The multicore per-core
    slices read `--input ${CMAKE_CURRENT_SOURCE_DIR}/../board.yaml` and resolve
    the SDK root four levels up; only the script token is constrained."""
    _write(tmp_path, "examples/multicore/rpmsg-aen/m55_hp/CMakeLists.txt", """\
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)

if(DEFINED ENV{ALP_SDK_ROOT})
    set(ALP_SDK_ROOT $ENV{ALP_SDK_ROOT})
else()
    get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../.. ABSOLUTE)
endif()

execute_process(
    COMMAND ${Python3_EXECUTABLE} ${ALP_SDK_ROOT}/scripts/alp_project.py
            --input ${CMAKE_CURRENT_SOURCE_DIR}/../board.yaml
            --emit zephyr-conf --core m55_hp
    RESULT_VARIABLE _alp_rv
)
""")
    assert find_problems(tmp_path) == []


def test_single_line_invocation_with_relative_input(tmp_path):
    """Script token and a `..` --input on ONE line: still legal. Guards against
    a line-level (rather than token-level) `..` test regressing the gate."""
    _write(tmp_path, "examples/multicore/mproc-mailbox/peer/CMakeLists.txt", """\
cmake_minimum_required(VERSION 3.20)
execute_process(COMMAND ${Python3_EXECUTABLE} ${ALP_SDK_ROOT}/scripts/alp_project.py --input ${CMAKE_CURRENT_SOURCE_DIR}/../board.yaml)
""")
    assert find_problems(tmp_path) == []


def test_absolute_path_also_fails(tmp_path):
    """A hardcoded absolute path is unportable for the same reason and is
    flagged too -- the invariant is `${ALP_SDK_ROOT}`, not merely `no ..`."""
    _write(tmp_path, "examples/ai/cold-chain-monitor/CMakeLists.txt", """\
cmake_minimum_required(VERSION 3.20)
execute_process(COMMAND ${Python3_EXECUTABLE} /opt/alp-sdk/scripts/alp_project.py)
""")
    problems = find_problems(tmp_path)
    assert len(problems) == 1
    assert "/opt/alp-sdk/scripts/alp_project.py" in problems[0]


def test_every_offender_is_reported(tmp_path):
    """Two bad examples -> two problems, one per file (the gate reports the
    whole set, so a sweep fixes them in one pass)."""
    _write(tmp_path, "examples/ai/a/CMakeLists.txt", BAD)
    _write(tmp_path, "examples/display/b/CMakeLists.txt", BAD)
    _write(tmp_path, "examples/peripheral-io/ok/CMakeLists.txt", GOOD)
    problems = find_problems(tmp_path)
    assert len(problems) == 2
    assert any("examples/ai/a/" in p for p in problems)
    assert any("examples/display/b/" in p for p in problems)
