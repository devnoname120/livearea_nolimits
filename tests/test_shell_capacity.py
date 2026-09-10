#!/usr/bin/env python3
"""Execute shell capacity functions using the production patch manifest and ELF.

Requires Unicorn 2.x and private firmware inputs. The manifest is emitted by
--manifest in the host startup test; tests/run.py manages it automatically when
LIVEAREA_TEST_PLUGIN_ELF is set. No firmware images are distributed.
"""
from pathlib import Path
import argparse
import json

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import (
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
    UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
    UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_CPSR,
)

from test_recovery_firmware import elf_symbols, signed

STACK = 0x70000000
OUTPUT = STACK + 0x1000
RETURN = STACK + 0x10000
REGISTERS = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
             UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7)
COUNTS = tuple(range(4003)) + (32767, 32768, 65535, 65536, 0x7FFFFFFF)
PROFILE_OFFSETS = {
    0x0552F692: (0x81000000, 0x552C6, 0x63A8E),
    0x5549BF1F: (0x81000000, 0x5531E, 0x63AE6),
    0xEAB89D5C: (0x83200DC0, 0x54E86, 0x6364E),
}


class NativeShell:
    def __init__(self, text: bytes, base: int, admission: int, message: int,
                 pages: int):
        self.cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        first = base & ~0xFFF
        length = (base - first + len(text) + 0xFFF) & ~0xFFF
        self.cpu.mem_map(first, length)
        self.cpu.mem_write(base, text)
        self.cpu.mem_map(STACK, 0x11000)
        self.admission = base + admission - 0x46
        self.message = base + message - 0x2E
        self.pages = pages
        self.counted = self.top_level = 0
        self.calls = []
        self.imports = {
            self.admission + 0x1A: "init",
            self.admission + 0x20: "counted",
            self.admission + 0x32: "top_level",
            self.admission + 0x3E: "end",
            self.admission + 0x5E: "end",
            self.message + 0x20: "init",
            self.message + 0x26: "counted",
            self.message + 0x5A: "end",
        }
        self.cpu.hook_add(UC_HOOK_CODE, self.imported_call)

    def imported_call(self, cpu: Uc, address: int, size: int, _: object) -> None:
        name = self.imports.get(address)
        if name is None:
            return
        self.calls.append(name)
        assert size == 4, "Expected the verified LSDB call instruction"
        if name == "top_level":
            assert cpu.reg_read(UC_ARM_REG_R1) == 0
            assert cpu.reg_read(UC_ARM_REG_R2) == self.pages
            cpu.reg_write(UC_ARM_REG_R0, self.top_level & 0xFFFFFFFF)
        elif name == "counted":
            cpu.reg_write(UC_ARM_REG_R0, self.counted & 0xFFFFFFFF)
        elif name == "end":
            cpu.reg_write(UC_ARM_REG_R0, 0)
        cpu.reg_write(UC_ARM_REG_PC, (address + 4) | 1)

    def call(self, entry: int, argument: int) -> int:
        self.calls = []
        self.cpu.reg_write(UC_ARM_REG_CPSR, 0x30)
        for i, register in enumerate(REGISTERS):
            self.cpu.reg_write(register, 0x12340000 + i)
        self.cpu.reg_write(UC_ARM_REG_R0, argument & 0xFFFFFFFF)
        self.cpu.reg_write(UC_ARM_REG_SP, STACK + 0xF000)
        self.cpu.reg_write(UC_ARM_REG_LR, RETURN | 1)
        self.cpu.mem_write(OUTPUT, b"\xA5" * 8)
        self.cpu.emu_start(entry | 1, RETURN, count=300)
        assert self.cpu.reg_read(UC_ARM_REG_PC) == RETURN, "Native function did not return"
        assert self.cpu.reg_read(UC_ARM_REG_SP) == STACK + 0xF000
        for i in range(4, 8):
            assert self.cpu.reg_read(REGISTERS[i]) == 0x12340000 + i
        return signed(self.cpu.reg_read(UC_ARM_REG_R0))

    def check_admission(self, top: int, counted: int, top_limit: int,
                        counted_limit: int) -> None:
        self.top_level, self.counted = top, counted
        result = self.call(self.admission, OUTPUT)
        expected = 0 if top < 0 else int(top < top_limit and counted < counted_limit)
        assert result == (top if top < 0 else 0), (top, counted, result)
        assert bytes(self.cpu.mem_read(OUTPUT, 8)) == bytes([expected]) + b"\xA5" * 7
        assert self.calls == ["init", "counted", "top_level", "end"]

    def check_message(self, count: int, top_limit: int, counted_limit: int) -> None:
        self.counted = count
        result = self.call(self.message, 0x80101113) & 0xFFFFFFFF
        expected = (0x9B75504E if top_limit < count < counted_limit else
                    0xB7077687 if count == counted_limit else 0x4502AFC3)
        assert result == expected, (count, hex(result), hex(expected))
        assert self.calls == ["init", "counted", "end"]


def check_page_instructions(cpu: Uc, base: int, patches: list[dict], limit: int) -> int:
    checks = 0
    for patch in patches:
        name = patch["symbol"]
        if not name.endswith("_page_limit"):
            continue
        register_number = int(name.split("_")[2][1:])
        register = REGISTERS[register_number]
        for pages in (0, 9, 10, 25, 26, 49, 50, 51, 127, 128, 255):
            cpu.reg_write(UC_ARM_REG_CPSR, 0x30)
            cpu.reg_write(register, pages)
            cpu.emu_start((base + patch["offset"]) | 1, RETURN, count=1)
            assert cpu.reg_read(UC_ARM_REG_PC) == base + patch["offset"] + patch["size"]
            if name.startswith("patch_movs"):
                assert cpu.reg_read(register) == limit
            else:
                flags = cpu.reg_read(UC_ARM_REG_CPSR)
                assert bool(flags & (1 << 30)) == (pages == limit)
                assert bool(flags & (1 << 29)) == (pages >= limit)
            checks += 1
    assert checks == 121, "Every one of the eleven page-limit sites must be tested"
    return checks


def check_image(path: Path, profile: dict, manifest: dict,
                symbols: dict[str, bytes]) -> int:
    nid = profile["nid"]
    base, admission, message = PROFILE_OFFSETS[nid]
    stock = path.read_bytes()
    assert len(stock) == profile["text_size"]
    assert len(profile["patches"]) == 22
    modified = bytearray(stock)
    used = set()
    wide = {}
    for patch in profile["patches"]:
        offset, size, symbol = patch["offset"], patch["size"], patch["symbol"]
        expected = bytes.fromhex(patch["expected"])
        replacement = symbols[symbol]
        assert 0 < size == len(expected) == len(replacement) <= 22
        assert 0 <= offset <= len(stock) - size
        assert stock[offset:offset + size] == expected, (hex(nid), hex(offset))
        locations = set(range(offset, offset + size))
        assert not used.intersection(locations), "Overlapping production patches"
        used.update(locations)
        modified[offset:offset + size] = replacement
        if size > 4:
            wide[symbol] = (offset, size)
    assert wide == {"patch_top_level_admission": (admission, 22),
                    "patch_top_level_message": (message, 12)}
    checks = 0
    for text, pages, top_limit, counted_limit in (
        (stock, 10, 100, 500),
        (bytes(modified), manifest["pages"], manifest["top_level"], manifest["counted"]),
    ):
        shell = NativeShell(text, base, admission, message, pages)
        for count in COUNTS:
            for counted in (499, 500, 999, 1000, 1999, 2000, 3999, 4000, 4001):
                shell.check_admission(count, counted, top_limit, counted_limit)
                checks += 1
            shell.check_message(count, top_limit, counted_limit)
            checks += 1
        for count in (-1, -123, -0x7FFFFFFF):
            shell.check_admission(count, 500, top_limit, counted_limit)
            shell.check_message(count, top_limit, counted_limit)
            checks += 2
        checks += check_page_instructions(shell.cpu, base, profile["patches"], pages)
    print(f"Native shell 0x{nid:08X}: {checks} checks; complete admission/message functions, "
          "255/256 and 499/500 boundaries, query errors, eleven page-limit sites passed")
    return checks


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("plugin_elf", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("inputs", nargs="+")
    args = parser.parse_args()
    assert len(args.inputs) % 2 == 0, "Expected NID/text pairs"
    manifest = json.loads(args.manifest.read_text())
    assert (manifest["pages"], manifest["top_level"], manifest["counted"]) == (50, 500, 4000)
    profiles = {p["nid"]: p for p in manifest["profiles"]}
    symbols = elf_symbols(args.plugin_elf)
    checks = 0
    for i in range(0, len(args.inputs), 2):
        nid = int(args.inputs[i], 0)
        checks += check_image(Path(args.inputs[i + 1]), profiles[nid], manifest, symbols)
    print(f"Native shell capacity: {checks} checks passed; external LSDB calls are stubbed, "
          "not a full Vita UI/database test")


if __name__ == "__main__":
    main()
