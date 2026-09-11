#!/usr/bin/env python3
"""Run host startup tests; optionally pass pairs of module NID and text dump.

Example: python3 tests/run.py 0x5549BF1F /path/to/SceShell.text.bin
Firmware images are local inputs and are not distributed with the tests.
"""
from pathlib import Path
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
subprocess.run([sys.executable, str(root / "tests/run_instance_cache.py")], check=True)
subprocess.run([sys.executable, str(root / "tests/test_recovery_hook_scope.py")], check=True)
with tempfile.TemporaryDirectory(prefix="livearea-tests-") as temporary:
    work = Path(temporary)
    # These tests exercise startup/rollback on the host. Thumb replacement
    # instructions are assembled and inspected separately in the Vita build.
    declarations = dict(re.findall(r"extern const uint8_t (patch_\w+)\[(\d+)\];",
                                   "\n".join((root / name).read_text() for name in
                                             ("src/main.c", "src/recovery.c"))))
    replacements = work / "replacements.c"
    replacements.write_text("#include <stdint.h>\n" + "\n".join(
        f"const uint8_t {name}[{size}] = {{0}};" for name, size in declarations.items()) +
        "\nconst char *test_patch_name(const uint8_t *data) {\n" + "\n".join(
            f'if (data == {name}) return "{name}";' for name in declarations) +
        '\nreturn "unknown";\n}\n')
    binary = work / "test_startup"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-I", str(root / "tests/stubs"),
        str(root / "tests/test_startup.c"), str(replacements),
        "-o", str(binary)], check=True)
    subprocess.run([str(binary), *sys.argv[1:]], check=True)
    manifest = work / "shell-patches.json"
    manifest.write_bytes(subprocess.check_output([str(binary), "--manifest"]))
    integration = work / "test_startup_with_trial"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-DLIVEAREA_ICON_CACHE_TRIAL=1",
        "-I", str(root / "tests/stubs"), str(root / "tests/test_startup.c"),
        str(replacements), "-o", str(integration)], check=True)
    subprocess.run([str(integration), *sys.argv[1:]], check=True)
    recovery = work / "test_recovery"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-I", str(root / "tests/stubs"),
        str(root / "tests/test_recovery.c"), str(replacements),
        "-o", str(recovery)], check=True)
    shell_inputs = {int(sys.argv[i], 0): sys.argv[i + 1]
                    for i in range(1, len(sys.argv), 2)}
    recovery_paths = {
        suffix: os.environ.get(f"LIVEAREA_TEST_RECOVERY_{suffix}")
        for suffix in ("360", "365")
    }
    recovery_inputs = []
    for nid, suffix in (
        (0x0552F692, "360"),
        (0x5549BF1F, "365"),
        (0xEAB89D5C, "360"),
    ):
        path = recovery_paths[suffix]
        shell = shell_inputs.get(nid)
        if path and shell:
            recovery_inputs.extend((hex(nid), shell, path))
        elif path and nid != 0xEAB89D5C:
            raise SystemExit(f"{path} requires shell text for 0x{nid:08X}")
    subprocess.run([str(recovery), *recovery_inputs], check=True)
    paf_text = os.environ.get("LIVEAREA_TEST_PAF_TEXT")
    cache_inputs = []
    for nid, variable, init_offset in (
        (0x0552F692, "LIVEAREA_TEST_PAF_TEXT", "0x2C74"),
        (0x5549BF1F, "LIVEAREA_TEST_PAF_365_TEXT", "0x2CCC"),
        (0xEAB89D5C, "LIVEAREA_TEST_PAF_PTEL_TEXT", "0x2C74"),
    ):
        paf = os.environ.get(variable)
        shell = next((sys.argv[i + 1] for i in range(1, len(sys.argv), 2)
                      if int(sys.argv[i], 0) == nid), None)
        cache_inputs.append((nid, paf, shell, init_offset))
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
            if name == "test_icon_cache_trial":
                profiles = json.loads(subprocess.check_output([str(binary), "--profiles"]))
                shell_profiles = {profile["nid"] for profile in json.loads(manifest.read_text())["profiles"]}
                assert len(profiles) == len(set(profiles))
                assert set(profiles) == shell_profiles, "Every supported shell must have a cache profile"
                assert {nid for nid, _, _, _ in cache_inputs} == shell_profiles
                for nid, paf, shell, _ in cache_inputs:
                    inputs = [hex(nid)]
                    if paf:
                        inputs.append(paf)
                        if shell:
                            inputs.append(shell)
                    subprocess.run([str(binary), *inputs], check=True)
            else:
                subprocess.run([str(binary), *arguments], check=True)

    for sync_interval in (1, 8):
        debug_log = work / f"test_debug_log-{sync_interval}"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
            "-fsanitize=address,undefined", "-DLIVEAREA_DEBUG_LOGGING=1",
            '-DLIVEAREA_DEBUG_BUILD_ID="test-build"',
            f"-DLIVEAREA_DEBUG_SYNC_INTERVAL={sync_interval}",
            "-I", str(root / "tests/stubs"),
            str(root / "tests/test_debug_log.c"), str(root / "src/debug_log.c"),
            "-o", str(debug_log)], check=True)
        subprocess.run([str(debug_log)], check=True)

    debug_startup = work / "test_startup_debug"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-DLIVEAREA_ICON_CACHE_TRIAL=1",
        "-DLIVEAREA_DEBUG_LOGGING=1", "-I", str(root / "tests/stubs"),
        str(root / "tests/test_startup.c"), str(root / "tests/debug_capture.c"),
        str(replacements), "-o", str(debug_startup)], check=True)
    subprocess.run([str(debug_startup), *sys.argv[1:]], check=True)

    debug_recovery = work / "test_recovery_debug"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-DLIVEAREA_DEBUG_LOGGING=1",
        "-I", str(root / "tests/stubs"), str(root / "tests/test_recovery.c"),
        str(root / "tests/debug_capture.c"), str(replacements),
        "-o", str(debug_recovery)], check=True)
    subprocess.run([str(debug_recovery), *recovery_inputs], check=True)

    debug_cache = work / "test_icon_cache_debug"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-DLIVEAREA_DEBUG_LOGGING=1",
        "-I", str(root / "tests/stubs"),
        str(root / "tests/test_icon_cache_trial.c"),
        str(root / "tests/debug_capture.c"), "-o", str(debug_cache)], check=True)
    for nid, paf, shell, _ in cache_inputs:
        inputs = [hex(nid)]
        if paf:
            inputs.append(paf)
            if shell:
                inputs.append(shell)
        subprocess.run([str(debug_cache), *inputs], check=True)

    debug_consumer = work / "test_icon_consumer_debug"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-DLIVEAREA_DEBUG_LOGGING=1",
        "-I", str(root / "tests/stubs"), str(root / "tests/test_icon_consumer.c"),
        str(root / "tests/debug_capture.c"), "-o", str(debug_consumer)], check=True)
    subprocess.run([str(debug_consumer)], check=True)

    substitute_path = os.environ.get("LIVEAREA_TEST_SUBSTITUTE")
    if substitute_path:
        if not any(paf and shell for _, paf, shell, _ in cache_inputs):
            raise SystemExit("LIVEAREA_TEST_SUBSTITUTE requires a matching PAF and shell text pair")
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
        hook_test = work / "test_hook_transform"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-DFORCE_TARGET_arm",
            "-I", str(substitute / "lib"), "-I", str(substitute / "generated"),
            str(root / "tests/test_hook_transform.c"),
            str(substitute / "lib/jump-dis.c"),
            str(substitute / "lib/cbit/vec.c"),
            str(substitute / "lib/strerror.c"), "-o", str(hook_test)], check=True)
        for nid, paf, shell, init_offset in cache_inputs:
            if paf and shell:
                print(f"Cache relocator profile: 0x{nid:08X}", flush=True)
                subprocess.run([str(hook_test), paf, offsets["PAF_EVICT_OFFSET"],
                                offsets["PAF_SCAN_OFFSET"], shell, init_offset,
                                offsets["PAF_APPLY_OFFSET"]], check=True)
        recovery_offsets = dict(re.findall(
            r"#define\s+(SHELL_RECOVERY_READY_OFFSET)\s+(0x[0-9A-Fa-f]+)U",
            (root / "src/recovery.c").read_text()))
        recovery_hook_test = work / "test_recovery_hook_transform"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-DFORCE_TARGET_arm",
            "-I", str(substitute / "lib"), "-I", str(substitute / "generated"),
            str(root / "tests/test_recovery_hook_transform.c"),
            str(substitute / "lib/jump-dis.c"),
            str(substitute / "lib/cbit/vec.c"),
            str(substitute / "lib/strerror.c"),
            "-o", str(recovery_hook_test)], check=True)
        for nid, _, shell, _ in cache_inputs:
            if shell:
                print(f"Recovery callback relocator profile: 0x{nid:08X}", flush=True)
                subprocess.run([str(recovery_hook_test), shell,
                                recovery_offsets["SHELL_RECOVERY_READY_OFFSET"]],
                               check=True)
    arm_python = os.environ.get("LIVEAREA_TEST_ARM_PYTHON")
    if arm_python:
        subprocess.run([arm_python, str(root / "tests/test_recovery_stop_redirect.py")],
                       check=True)
    plugin_elf = os.environ.get("LIVEAREA_TEST_PLUGIN_ELF")
    if plugin_elf:
        arm_python = arm_python or sys.executable
        if not sys.argv[1:]:
            raise SystemExit("Native shell tests require local shell NID/text pairs")
        subprocess.run([arm_python, str(root / "tests/test_shell_capacity.py"),
                        plugin_elf, str(manifest), *sys.argv[1:]], check=True)
        firmware_recovery_paths = [path for path in recovery_paths.values() if path]
        if firmware_recovery_paths:
            subprocess.run([arm_python, str(root / "tests/test_recovery_firmware.py"),
                            plugin_elf, *firmware_recovery_paths], check=True)
