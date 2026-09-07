#!/usr/bin/env python3
"""Execute local retail recovery instructions with assembled plugin replacements.

Requires Unicorn 2.x. Firmware images are private test inputs, not fixtures.
Usage: python3 tests/test_recovery_firmware.py PLUGIN_ELF RECOVERY_TEXT [...]
"""
from pathlib import Path
import argparse
import struct

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import (
    UC_ARM_REG_R0, UC_ARM_REG_R4, UC_ARM_REG_R9,
    UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC,
)

BASE = 0x81000000
STACK = 0x70000000
RETURN = STACK + 0x10000
PATCHES = (
    (0x06280, bytes.fromhex("b0 f5 fa 7f"), "patch_cmp_r0_icon_limit"),
    (0x07054, bytes.fromhex("b0 f5 fa 7f"), "patch_cmp_r0_icon_limit"),
    (0x0C700, bytes.fromhex("d0 f5 fa 71"), "patch_rsbs_r1_r0_icon_limit"),
    (0x0C776, bytes.fromhex("d0 f5 fa 71"), "patch_rsbs_r1_r0_icon_limit"),
    (0x0C834, bytes.fromhex("d0 f5 fa 71"), "patch_rsbs_r1_r0_icon_limit"),
    (0x06084, bytes.fromhex("0a 2c"), "patch_cmp_r4_page_limit"),
    (0x060FE, bytes.fromhex("5f f0 09 09"), "patch_movs_r9_last_page"),
)


def elf_symbols(path: Path) -> dict[str, bytes]:
    data = path.read_bytes()
    assert data[:7] == b"\x7fELF\x01\x01\x01", "Expected little-endian ELF32"
    header = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
    assert header[2] == 40 and header[11] == 40, "Expected ARM ELF32 sections"
    sections = [struct.unpack_from("<10I", data, header[6] + 40 * i)
                for i in range(header[12])]
    symbols = {}
    for section in sections:
        if section[1] != 2:
            continue
        assert section[9] == 16
        names = sections[section[6]]
        strings = data[names[4]:names[4] + names[5]]
        for offset in range(section[4], section[4] + section[5], 16):
            name, address, size, _, _, index = struct.unpack_from("<IIIBBH", data, offset)
            if not name or not size or not 0 < index < len(sections):
                continue
            text = strings[name:strings.index(b"\0", name)].decode("ascii")
            if not text.startswith("patch_"):
                continue
            owner = sections[index]
            start = owner[4] + address - owner[3]
            assert owner[4] <= start <= owner[4] + owner[5] - size
            symbols[text] = data[start:start + size]
    return symbols


def emulator(text: bytes) -> Uc:
    machine = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    machine.mem_map(BASE, 0x43000)
    machine.mem_write(BASE, text)
    machine.mem_map(STACK, 0x11000)
    machine.reg_write(UC_ARM_REG_SP, STACK + 0xF000)
    machine.reg_write(UC_ARM_REG_LR, RETURN | 1)
    return machine


def signed(value: int) -> int:
    return value - 0x100000000 if value & 0x80000000 else value


def native_capacity(text: bytes, entry: int, displayed: int, hidden: int,
                    error: int = 0) -> int:
    machine = emulator(text)
    imports = {
        BASE + 0x2AF44: 0,
        BASE + 0x2AF24: hidden,
        BASE + 0x2AD44: displayed,
        BASE + 0x2AD54: 0,
        BASE + 0x2AE64: error,
    }
    calls = []

    def imported_call(cpu: Uc, address: int, size: int, user_data: object) -> None:
        if address in imports:
            calls.append(address)
            cpu.reg_write(UC_ARM_REG_R0, imports[address] & 0xFFFFFFFF)
            cpu.reg_write(UC_ARM_REG_PC, cpu.reg_read(UC_ARM_REG_LR))
        elif address == BASE + 0x2B184:
            raise AssertionError("Unexpected stack-guard failure")

    machine.hook_add(UC_HOOK_CODE, imported_call)
    machine.emu_start(BASE + entry | 1, RETURN, count=1000)
    assert machine.reg_read(UC_ARM_REG_PC) == RETURN, "Native function did not return"
    assert calls, "Did not exercise LSDB queries"
    return signed(machine.reg_read(UC_ARM_REG_R0))


def branch_falls_through(text: bytes, offset: int, register: int, value: int,
                         fallthrough: int) -> bool:
    machine = emulator(text)
    machine.reg_write(register, value)
    machine.emu_start(BASE + offset | 1, RETURN, count=2)
    return machine.reg_read(UC_ARM_REG_PC) == BASE + fallthrough


def check_image(path: Path, symbols: dict[str, bytes]) -> None:
    stock = path.read_bytes()
    assert len(stock) == 0x3E9E8
    nid = struct.unpack_from("<I", stock, 0x2C294 + 52)[0]
    assert nid in (0xC1F30F67, 0x3F76E38F)
    modified = bytearray(stock)
    for offset, original, symbol in PATCHES:
        replacement = symbols[symbol]
        assert len(replacement) == len(original)
        assert stock[offset:offset + len(original)] == original
        modified[offset:offset + len(original)] = replacement
    patched = bytes(modified)
    for text, limit in ((stock, 500), (patched, 1000)):
        for displayed in (0, 499, 500, 501, 999, 1000):
            if displayed > limit:
                continue
            for hidden in (0, 1, 2, 1001):
                remaining = min(hidden, limit - displayed)
                assert native_capacity(text, 0xC6B4, displayed, hidden) == remaining
                assert native_capacity(text, 0xC724, displayed, hidden) == (-1 if remaining else 0)
                assert native_capacity(text, 0xC7A4, displayed, hidden) == (0x80000 if remaining else 0)
        for displayed, hidden in ((-1, 2), (0, -1)):
            assert native_capacity(text, 0xC6B4, displayed, hidden) == 0
        for count in (499, 500, 501, 999, 1000, 1001):
            assert branch_falls_through(text, 0x6280, UC_ARM_REG_R0, count, 0x6288) == (count < limit)
            assert branch_falls_through(text, 0x7054, UC_ARM_REG_R0, count, 0x705A) == (count >= limit)
        assert native_capacity(text, 0xC724, 500, 2, -123) == -123
    assert native_capacity(stock, 0xC6B4, 500, 2) == 0
    assert native_capacity(patched, 0xC6B4, 500, 2) == 2
    for text, limit in ((stock, 10), (patched, 50)):
        for pages in (9, 10, 11, 25, 26, 27, 49, 50, 51):
            assert branch_falls_through(text, 0x6084, UC_ARM_REG_R4, pages, 0x6088) == (pages < limit)
        machine = emulator(text)
        machine.emu_start(BASE + 0x60FE | 1, RETURN, count=1)
        assert machine.reg_read(UC_ARM_REG_R9) == limit - 1
    print(f"Native ARM recovery passed: {path} (NID 0x{nid:08X})")
    print("  Stock 500+2 hidden => 0; assembled fix => 2; three native planning functions,")
    print("  two admission branches, page creation/search bounds and LSDB errors verified")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("plugin_elf", type=Path)
    parser.add_argument("recovery_text", nargs="+", type=Path)
    args = parser.parse_args()
    symbols = elf_symbols(args.plugin_elf)
    for path in args.recovery_text:
        check_image(path, symbols)


if __name__ == "__main__":
    main()
