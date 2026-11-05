# SPDX-License-Identifier: MIT
"""Resident ISP metadata follows the current ADT without starting the ISP."""
from pathlib import Path
import subprocess

from test_isp_heap import HARNESS, TESTS
from test_siliconforest_handoff import run_c

ROOT = Path(__file__).resolve().parents[2]


def definition(source, name):
    signature = "static int " + name + "("
    start = source.index(signature)
    while source.index(";", start) < source.index("{", start):
        start = source.index(signature, start + len(signature))
    return source[start:source.index("\n}", start) + 2] + "\n"


def test_t8142_never_powers_or_allocates_the_resident_isp(tmp_path):
    source = (ROOT / "src/isp.c").read_text()
    source = source[source.index("#define ISP_ASC_VERSION"):]
    reset = TESTS[:TESTS.index("static void success(void)")]
    binary = tmp_path / "isp-resident"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-format", "-Wno-unused-function", "-fsanitize=address,undefined",
                    "-x", "c", "-", "-o", str(binary)],
                   input=HARNESS + source + reset + r'''
int main(void)
{
    reset(); chip_id = T8142;
    /* Repeated DT preparation must not reuse a heap from another profile. */
    isp_initialized = true;
    assert(isp_init() == 0 && !isp_initialized);
    assert(!enabled && !disabled && !active && !gated && !allocations);
    assert(!read_address && !dapf_calls && !mode_calls);
    return 0;
}
''', text=True, check=True)
    subprocess.run([str(binary)], check=True)
