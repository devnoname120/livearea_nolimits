#!/usr/bin/env python3
"""Run the instance-cache ownership tests under ASan/UBSan."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="livearea-instance-tests-") as directory:
    capture = Path(directory) / "capture.o"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-DLIVEAREA_DEBUG_LOGGING=1",
        "-I", str(root / "tests/stubs"), "-c",
        str(root / "tests/debug_capture.c"), "-o", str(capture),
    ], check=True)
    for logging in (0, 1):
        binary = Path(directory) / f"instance-cache-{logging}"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
            "-fsanitize=address,undefined", f"-DLIVEAREA_DEBUG_LOGGING={logging}",
            "-I", str(root / "tests/stubs"),
            str(root / "tests/test_instance_cache.c"),
            str(capture), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
