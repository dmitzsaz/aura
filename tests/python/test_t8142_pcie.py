# SPDX-License-Identifier: GPL-2.0-only
"""Compile the complete T8142 producer against a gated MMIO model and real libfdt."""
from pathlib import Path
import shutil
import subprocess

import pytest


@pytest.fixture(scope="module")
def producer(tmp_path_factory):
    repo = Path(__file__).resolve().parents[2]
    work = tmp_path_factory.mktemp("t8142-pcie")
    for name in ("pcie_t8142.c", "pcie.h", "adt.h", "tunables.h", "kboot_nvme.c", "kboot_nvme.h"):
        shutil.copy(repo / "src" / name, work)
    for header in (repo / "tests/t8142_pcie_host").glob("*.h"):
        shutil.copy(header, work)
    shutil.copy(repo / "tests/t8142_pcie_host.c", work)
    binary = work / "producer"
    libfdt = repo / "src/libfdt"
    subprocess.run([
        "cc", "-std=gnu11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter", "-Wno-sign-compare", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", f"-I{work}", f"-I{repo / 'src'}",
        str(work / "t8142_pcie_host.c"),
        *[str(libfdt / name) for name in (
            "fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_wip.c", "fdt_sw.c", "fdt_empty_tree.c",
            "fdt_strerror.c", "fdt_check.c", "fdt_addresses.c")], "-o", str(binary),
    ], check=True)
    return binary


@pytest.mark.parametrize("scenario", range(20))
def test_producer(producer, scenario):
    subprocess.run([str(producer), str(scenario)], check=True)
