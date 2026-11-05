# SPDX-License-Identifier: MIT
"""Run the HAL300 transaction with real libfdt and the production SHA-256 shim.

Only the image allowlist is injected: tests use synthetic TEXT and the same
normalization and transaction as the production entry point. No owner data.
"""
from pathlib import Path
import json
import shutil
import subprocess

import pytest


@pytest.fixture(scope="module")
def producer(tmp_path_factory):
    if shutil.which("cargo") is None:
        pytest.skip("cargo is required for the production SHA-256 shim")
    repo = Path(__file__).resolve().parents[2]
    work = tmp_path_factory.mktemp("t8142-gpu")
    for name in ("kboot_gpu_t8142.c", "kboot_gpu_t8142.h", "adt.h", "firmware.h", "exception.h"):
        shutil.copy(repo / "src" / name, work)
    for name in ("types.h", "utils.h", "malloc.h"):
        shutil.copy(repo / "tests/t8142_pcie_host" / name, work)
    shutil.copy(repo / "tests/t8142_gpu_host.c", work)
    # Use the exact Rust FFI source, rather than a fake digest function in the host model.
    (work / "Cargo.toml").write_text('''[package]
name = "gpu-fwid-host"
version = "0.0.0"
edition = "2021"
[lib]
path = "lib.rs"
crate-type = ["staticlib"]
[dependencies]
sha2 = { version = "0.10", default-features = false }
''')
    rust_path = json.dumps(str(repo / "rust/src/gpu_fwid.rs"))
    (work / "lib.rs").write_text(f'#[path={rust_path}] mod gpu_fwid;\n')
    subprocess.run(["cargo", "build", "--release", "--manifest-path", str(work / "Cargo.toml")], check=True)
    source = (repo / "src/kboot_gpu.c").read_text()
    start = source.index("static int dt_set_resvmem(")
    reserve = source[start:source.index("\n}", start) + 2]
    start = source.index("/* G17 SoCs")
    legacy = source[start:source.index("/*\n * T6040", start)]
    start = source.index("int dt_set_gpu(void *dt)\n{")
    dispatch = source[start:source.index("    if (chip_id == T6040)", start)]
    (work / "legacy.inc").write_text(reserve + "\n" + legacy + "\n" + dispatch + "    abort();\n}\n")
    binary = work / "producer"
    libfdt = repo / "src/libfdt"
    subprocess.run([
        "cc", "-std=gnu11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter", "-Wno-sign-compare", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", f"-I{work}", f"-I{repo / 'src'}",
        str(work / "t8142_gpu_host.c"),
        *[str(libfdt / name) for name in (
            "fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_wip.c", "fdt_sw.c", "fdt_empty_tree.c",
            "fdt_strerror.c", "fdt_check.c", "fdt_addresses.c")],
        str(work / "target/release/libgpu_fwid_host.a"), "-lpthread", "-ldl", "-lm",
        "-o", str(binary),
    ], check=True)
    return binary


@pytest.mark.parametrize("scenario", range(48))
def test_handoff(producer, scenario):
    subprocess.run([str(producer), str(scenario)], check=True)
