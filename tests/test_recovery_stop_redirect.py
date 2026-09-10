from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB
from unicorn.arm_const import UC_ARM_REG_CPSR, UC_ARM_REG_PC


def run_case(base: int) -> None:
    entry = base + 0x1E
    target = base + 0x2001
    code = bytes.fromhex("DF F8 04 F0 00 BF") + target.to_bytes(4, "little")
    emulator = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    emulator.mem_map(base, 0x4000)
    emulator.mem_write(entry, code)
    emulator.reg_write(UC_ARM_REG_PC, entry | 1)
    emulator.emu_start(entry | 1, entry + 0x20, count=1)
    pc = emulator.reg_read(UC_ARM_REG_PC)
    cpsr = emulator.reg_read(UC_ARM_REG_CPSR)
    assert pc == (target & ~1), (hex(base), hex(pc), hex(target))
    assert cpsr & 0x20, hex(cpsr)


for address in (0x81000000, 0x82DC0000):
    run_case(address)

print("Recovery stop redirect: unaligned Thumb literal branch passed")
