#!/usr/bin/env python3
"""Run host startup tests; optionally pass pairs of module NID and text dump.

Example: python3 tests/run.py 0x5549BF1F /path/to/SceShell.text.bin
Firmware images are local inputs and are not distributed with the tests.
"""
from pathlib import Path
import os
import re
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="livearea-tests-") as temporary:
    work = Path(temporary)
    # These tests exercise startup/rollback on the host. Thumb replacement
    # instructions are assembled and inspected separately in the Vita build.
    declarations = re.findall(r"extern const uint8_t (patch_\w+)\[(\d+)\];",
                              (root / "src/main.c").read_text())
    replacements = work / "replacements.c"
    replacements.write_text("#include <stdint.h>\n" + "\n".join(
        f"const uint8_t {name}[{size}] = {{0}};" for name, size in declarations))
    binary = work / "test_startup"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-I", str(root / "tests/stubs"),
        str(root / "tests/test_startup.c"), str(replacements),
        "-o", str(binary)], check=True)
    subprocess.run([str(binary), *sys.argv[1:]], check=True)
