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
    integration = work / "test_startup_with_trial"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-DLIVEAREA_ICON_CACHE_TRIAL=1",
        "-I", str(root / "tests/stubs"), str(root / "tests/test_startup.c"),
        str(replacements), "-o", str(integration)], check=True)
    subprocess.run([str(integration), *sys.argv[1:]], check=True)
    paf_text = os.environ.get("LIVEAREA_TEST_PAF_TEXT")
    for logging in (False, True):
        definitions = ["-DLIVEAREA_ICON_CACHE_LOGGING=1"] if logging else []
        for name, arguments in (
            ("test_icon_cache_trial", [paf_text] if paf_text else []),
            ("test_icon_consumer", []),
        ):
            binary = work / f"{name}-logging-{int(logging)}"
            subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
                "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                "-fsanitize=address,undefined", *definitions,
                "-I", str(root / "tests/stubs"),
                str(root / "tests" / f"{name}.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary), *arguments], check=True)
    substitute_path = os.environ.get("LIVEAREA_TEST_SUBSTITUTE")
    if substitute_path:
        if not paf_text:
            raise SystemExit("LIVEAREA_TEST_SUBSTITUTE requires LIVEAREA_TEST_PAF_TEXT")
        substitute = Path(substitute_path).resolve()
        revision = subprocess.run(
            ["git", "-C", str(substitute), "rev-parse", "HEAD"],
            capture_output=True, text=True, check=True).stdout.strip()
        if revision != "4713452731dd489c13c9415c3c31637845f108f3":
            raise SystemExit(f"Unexpected taiHEN libsubstitute revision: {revision}")
        subprocess.run(["git", "-C", str(substitute), "diff", "--quiet",
                        "HEAD", "--", "lib"], check=True)
        offsets = dict(re.findall(
            r"#define\s+((?:PAF|SHELL)_\w+_OFFSET)\s+(0x[0-9A-Fa-f]+)U",
            (root / "src/icon_cache_trial.c").read_text()))
        shell_text = next((sys.argv[i + 1] for i in range(1, len(sys.argv), 2)
                           if int(sys.argv[i], 0) == 0x0552F692), None)
        if not shell_text:
            raise SystemExit("Relocator checks require the retail 3.60 shell text argument")
        hook_test = work / "test_hook_transform"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-DFORCE_TARGET_arm",
            "-I", str(substitute / "lib"), "-I", str(substitute / "generated"),
            str(root / "tests/test_hook_transform.c"),
            str(substitute / "lib/jump-dis.c"),
            str(substitute / "lib/cbit/vec.c"),
            str(substitute / "lib/strerror.c"), "-o", str(hook_test)], check=True)
        subprocess.run([str(hook_test), paf_text, offsets["PAF_EVICT_OFFSET"],
                        offsets["PAF_SCAN_OFFSET"], shell_text,
                        offsets["SHELL_POOL_INIT_OFFSET"], offsets["PAF_APPLY_OFFSET"]], check=True)
