"""ELF loading and Thumb branch checks shared by offline ARM tests."""
from pathlib import Path
import struct
import sys
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn import arm_const as reg

HEAP, STACK, RETURN = 0x90000000, 0x90010000, 0x88000000


def elf(path):
    data = path.read_bytes()
    header = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
    assert data[:7] == b"\x7fELF\x01\x01\x01" and header[2] == 40
    segments = [struct.unpack_from("<8I", data, header[5] + i * header[9])
                for i in range(header[10])]
    sections = [struct.unpack_from("<10I", data, header[6] + i * header[11])
                for i in range(header[12])]
    symbols = {}
    for section in sections:
        if section[1] != 2:
            continue
        names = sections[section[6]]
        strings = data[names[4]:names[4] + names[5]]
        for offset in range(section[4], section[4] + section[5], section[9]):
            name, address, size, _, _, _ = struct.unpack_from("<IIIBBH", data, offset)
            if name:
                symbols[strings[name:strings.index(b"\0", name)].decode()] = address
    return data, [s for s in segments if s[0] == 1], symbols


def machine(plugin):
    data, segments, _ = plugin
    u = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    for s in segments:
        low = s[2] & ~0xFFF
        u.mem_map(low, (s[2] - low + s[5] + 0xFFF) & ~0xFFF)
        u.mem_write(s[2], data[s[1]:s[1] + s[4]])
    u.mem_map(HEAP, 0x10000)
    u.mem_map(STACK, 0x10000)
    u.mem_map(RETURN, 0x1000)
    u.reg_write(reg.UC_ARM_REG_CPSR, 0x30)
    # Vita user code may use VFP/NEON for small structure copies.
    u.reg_write(reg.UC_ARM_REG_C1_C0_2, 0x00F00000)
    u.reg_write(reg.UC_ARM_REG_FPEXC, 0x40000000)
    u.reg_write(reg.UC_ARM_REG_SP, STACK + 0xF000)
    return u


def word(u, address, value):
    u.mem_write(address, struct.pack("<I", value))


def encode(u, symbols, source, target):
    u.reg_write(reg.UC_ARM_REG_R0, HEAP + 0x8000)
    u.reg_write(reg.UC_ARM_REG_R1, source)
    u.reg_write(reg.UC_ARM_REG_R2, target)
    u.reg_write(reg.UC_ARM_REG_LR, RETURN | 1)
    u.emu_start(symbols["shell_detour_encode_branch"] | 1, RETURN, count=200)
    assert u.reg_read(reg.UC_ARM_REG_PC) == RETURN
    return u.reg_read(reg.UC_ARM_REG_R0), bytes(u.mem_read(HEAP + 0x8000, 4))


def check_encoder(plugin):
    u = machine(plugin)
    source = 0x81880002
    cases = 0
    for delta in [-16777216, -16777214, -4096, -2, 0, 2, 4094, 16777214]:
        target = source + 4 + delta
        result, code = encode(u, plugin[2], source, target | 1)
        assert result == 0
        # Execute the emitted instruction, even for a distant target. Stop at
        # that target before any instruction there needs to be meaningful.
        for address in {source & ~0xFFF, target & ~0xFFF}:
            try:
                u.mem_map(address, 0x1000)
            except Exception:
                assert any(lo <= address <= hi for lo, hi, _ in u.mem_regions())
        u.mem_write(source, code)
        u.ctl_remove_cache(source, source + 4)
        u.emu_start(source | 1, target, count=1)
        assert u.reg_read(reg.UC_ARM_REG_PC) == target, (delta, code.hex(), hex(target), hex(u.reg_read(reg.UC_ARM_REG_PC)))
        cases += 1
    for src, target in [(source, source + 4 - 16777218 | 1),
                        (source, source + 4 + 16777216 | 1),
                        (source | 1, source + 9), (source, source + 8),
                        (0xFFFFFFFE, 1)]:
        assert encode(u, plugin[2], src, target)[0] == 0xFFFFFFFF
        cases += 1
    return cases
