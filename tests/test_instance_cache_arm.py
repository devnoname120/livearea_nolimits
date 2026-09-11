#!/usr/bin/env python3
"""Execute the compiled instance backend with supported PAF code in Unicorn.

OS services, async decoding and widget rendering are mocked. Pool allocation,
surface reference release, the native widget consumer, compiled vtable callbacks
and entry replay execute as ARM code. This does not emulate a complete Vita.
Usage: python test_instance_cache_arm.py PLUGIN_ELF PAF360_TEXT PAF365_TEXT [PAFPTEL_TEXT]
Set LIVEAREA_INSTANCE_SHELL_TEXTS to a path-separated list of matching shell
text files to additionally execute startup, detour dispatch and rollback checks.
"""
from pathlib import Path
import struct
import sys
import os
from unicorn import UC_HOOK_CODE
from unicorn import arm_const as reg
from arm_helpers import elf, machine, word, encode, check_encoder, HEAP, STACK, RETURN

PAF = 0x81000000
DATA = 0x81301000
CACHE_MUTEX, SURFACE_MUTEX = DATA + 0xB238, DATA + 0x10230
POOL, ALLOC, CACHE, SLOT, ICON_POOL = [HEAP + n for n in (0, 0x100, 0x200, 0x300, 0x310)]
IMAGE, SURFACE, WIDGET, WVT, OBJECT, HANDLE_PTR = [HEAP + n for n in (0x1000, 0x2000, 0x3000, 0x3400, 0x3800, 0x3900)]


class Harness:
    def __init__(self, plugin, paf, firmware):
        global PAF, DATA, CACHE_MUTEX, SURFACE_MUTEX
        PAF, DATA = (0x83200E90, 0x811607E0) if firmware == 2 else (0x81000000, 0x81301000)
        CACHE_MUTEX, SURFACE_MUTEX = DATA + 0xB238, DATA + 0x10230
        self.u = u = machine(plugin)
        self.sym = plugin[2]
        self.firmware = firmware
        # The plugin's unrelocated ELF overlaps the first 0x50000 PAF bytes.
        # Only the exercised native ranges are mapped; GetSurface is an async
        # service mock. Its two-byte validation prefix is temporarily supplied.
        if firmware == 2:
            u.mem_map(PAF & ~0xFFF, 0x302000)
            u.mem_write(PAF, paf)
        else:
            u.mem_map(PAF + 0x50000, 0x2B1000)
            u.mem_write(PAF + 0x50000, paf[0x50000:])
        u.mem_map(DATA & ~0xFFF, 0x18000)
        self.depth = {}
        self.recursive = True
        self.contended = False
        self.free = 0
        self.binds = self.gets = self.destroyed = self.allocations = 0
        self.logs = []
        self.expected_args = None
        self.request_result = 0
        self.pressure = False
        self.injections = []
        self.injection_attempts = 0
        self.fail_injection = -1
        self.callbacks = {RETURN + 0x100: self.allocator,
                          RETURN + 0x120: self.surface_destructor,
                          RETURN + 0x140: self.bind,
                          RETURN + 0x160: lambda a: 0}
        word(u, self.sym['profile'], self.sym['profiles'] + firmware * 28)
        word(u, self.sym['icon_pool_slot'], SLOT)
        word(u, SLOT, ICON_POOL)
        word(u, ICON_POOL + 4, POOL)
        word(u, POOL, PAF + 0x2E3968)
        word(u, POOL + 4, ALLOC)
        word(u, POOL + 12, 2097152)
        word(u, ALLOC, ALLOC + 32)
        word(u, ALLOC + 40, RETURN + 0x101)
        word(u, CACHE + 84, POOL)
        word(u, DATA + 0xB224, 1000)
        word(u, self.sym['g_instance_request_resume'], RETURN + 0x501)
        word(u, self.sym['g_instance_pool_resume'], RETURN + 0x541)
        u.hook_add(UC_HOOK_CODE, self.step)

    def read(self, address):
        return struct.unpack('<I', self.u.mem_read(address, 4))[0]

    def cstring(self, address):
        return bytes(self.u.mem_read(address, 512)).split(b'\0', 1)[0].decode()

    def ret(self, value=0):
        self.u.reg_write(reg.UC_ARM_REG_R0, value & 0xFFFFFFFF)
        self.u.reg_write(reg.UC_ARM_REG_PC, self.u.reg_read(reg.UC_ARM_REG_LR))

    def lock(self, address, trying=False):
        depth = self.depth.get(address, 0)
        if trying and ((self.contended and address == CACHE_MUTEX) or (depth and not self.recursive)):
            return -1
        self.depth[address] = depth + 1
        return 0

    def unlock(self, address):
        assert self.depth.get(address, 0), hex(address)
        self.depth[address] -= 1
        return 0

    def allocator(self, args):
        assert args[0] == ALLOC and args[1] >= 16
        assert self.depth.get(POOL + 40) == 1
        self.allocations += 1
        if self.free < args[2]:
            return 0
        self.free -= args[2]
        return HEAP + 0x7000

    def surface_destructor(self, args):
        assert args[0] == SURFACE and self.read(SURFACE + 24) == 0
        self.destroyed += 1
        self.free += 64
        return 0

    def bind(self, args):
        assert args[0] == WIDGET and self.read(args[1]) == SURFACE
        assert self.read(SURFACE + 24) == 2
        word(self.u, SURFACE + 24, 3)  # Widget takes its own reference.
        self.binds += 1
        return 0

    def step(self, u, pc, size, _):
        args = [u.reg_read(getattr(reg, f'UC_ARM_REG_R{i}')) for i in range(4)]
        by_name = {address & ~1: name for name, address in self.sym.items()}
        name = by_name.get(pc)
        if name == 'taiGetModuleInfo':
            assert self.cstring(args[0]) == 'ScePaf'
            word(u, args[1] + 4, 123)
            word(u, args[1] + 8, [0xCD679177, 0x73F90499, 0xCD679177][self.firmware])
        elif name == 'sceKernelGetModuleInfo':
            assert args[0] == 123
            for i, (base, length) in enumerate([(PAF, 0x300D00), (DATA, 0x166D0)]):
                word(u, args[1] + 0x154 + i * 24 + 8, base)
                word(u, args[1] + 0x154 + i * 24 + 12, length)
        elif name == 'taiInjectDataForUser':
            values = [self.read(args[0] + i * 4) for i in range(7)]
            _, module, segment, offset, _, source, length = values
            assert module == 42 and segment == 0 and length == 4
            attempt = self.injection_attempts
            self.injection_attempts += 1
            if attempt == self.fail_injection:
                self.ret(-1); return
            address = self.shell_base + offset
            self.injections.append((address, bytes(u.mem_read(address, length))))
            u.mem_write(address, bytes(u.mem_read(source, length)))
            self.ret(100 + attempt); return
        elif name == 'taiInjectRelease':
            assert args[0] == 100 and len(self.injections) == 1
            address, previous = self.injections.pop()
            u.mem_write(address, previous)
        elif name in ('debug_logf', 'debug_log_flush'):
            if name == 'debug_logf':
                self.logs.append(self.cstring(args[1]))
        elif name == 'sceKernelGetThreadId':
            self.ret(101); return
        elif name == 'sceKernelTryLockLwMutex':
            assert args[1] == 1
            self.ret(self.lock(args[0], True)); return
        elif name == 'sceKernelUnlockLwMutex':
            assert args[1] == 1
            self.ret(self.unlock(args[0])); return
        elif name == 'sceClibMemset':
            u.mem_write(args[0], bytes([args[1] & 255]) * args[2])
            self.ret(args[0]); return
        elif name == 'sceClibMemcpy':
            u.mem_write(args[0], bytes(u.mem_read(args[1], args[2])))
            self.ret(args[0]); return
        elif pc in (PAF + 0x53E90, PAF + 0x53D50):
            self.ret(self.lock(args[0])); return
        elif pc in (PAF + 0x53EA4, PAF + 0x53D64):
            self.ret(self.unlock(args[0])); return
        elif pc == PAF + 0x8D90C:
            self.ret(self.free); return
        elif pc == PAF + 0x13344:
            assert args[1] == IMAGE and self.depth.get(CACHE_MUTEX)
            self.gets += 1
            surface = self.read(IMAGE + 156)
            word(u, args[0], surface)
            if surface:
                word(u, surface + 24, self.read(surface + 24) + 1)
            else:
                u.mem_write(IMAGE + 10, b'\x01')  # Async reload queued.
        elif pc in (PAF + 0x1405D4, PAF + 0x140908):
            pass  # Event construction/dispatch; no rendering in this harness.
        elif pc in self.callbacks:
            self.ret(self.callbacks[pc](args)); return
        elif pc == RETURN + 0x500:
            sp = u.reg_read(reg.UC_ARM_REG_SP)
            actual = args + [self.read(sp + 28)]
            if self.expected_args is not None:
                assert actual == self.expected_args, (actual, self.expected_args)
            word(u, args[1], IMAGE + 100)
            saved = [self.read(sp + i * 4) for i in range(7)]
            for i in range(6):
                u.reg_write(getattr(reg, f'UC_ARM_REG_R{i+4}'), saved[i])
            u.reg_write(reg.UC_ARM_REG_SP, sp + 28)
            u.reg_write(reg.UC_ARM_REG_R0, self.request_result & 0xFFFFFFFF)
            u.reg_write(reg.UC_ARM_REG_PC, saved[-1]); return
        elif pc == RETURN + 0x540:
            sp = u.reg_read(reg.UC_ARM_REG_SP)
            saved = [self.read(sp + i * 4) for i in range(6)]
            for i in range(5):
                u.reg_write(getattr(reg, f'UC_ARM_REG_R{i+4}'), saved[i])
            u.reg_write(reg.UC_ARM_REG_SP, sp + 24)
            u.reg_write(reg.UC_ARM_REG_PC, saved[-1]); return
        elif pc == PAF + 0x15FA12 and self.pressure:
            # Pause the real consumer between its status and getter. The test
            # then runs the real allocation callback on a second stack.
            self.pressure = False
            u.emu_stop(); return
        else:
            return
        self.ret()

    def call(self, address, *args):
        u = self.u
        if isinstance(address, str):
            address = self.sym[address]
        for i in range(4, 12):
            u.reg_write(getattr(reg, f'UC_ARM_REG_R{i}'), 0x12340000 + i)
        for i, value in enumerate(args[:4]):
            u.reg_write(getattr(reg, f'UC_ARM_REG_R{i}'), value)
        for i, value in enumerate(args[4:]):
            word(u, STACK + 0xF000 + i * 4, value)
        u.reg_write(reg.UC_ARM_REG_SP, STACK + 0xF000)
        u.reg_write(reg.UC_ARM_REG_LR, RETURN | 1)
        try:
            u.emu_start(address | 1, RETURN, count=100000)
        except Exception:
            pc = u.reg_read(reg.UC_ARM_REG_PC)
            print(f'ARM failure at {pc:#x}: {bytes(u.mem_read(pc, 8)).hex()}')
            raise
        if u.reg_read(reg.UC_ARM_REG_PC) == PAF + 0x15FA12:
            return None
        assert u.reg_read(reg.UC_ARM_REG_PC) == RETURN, hex(u.reg_read(reg.UC_ARM_REG_PC))
        assert u.reg_read(reg.UC_ARM_REG_SP) == STACK + 0xF000
        for i in range(4, 12):
            assert u.reg_read(getattr(reg, f'UC_ARM_REG_R{i}')) == 0x12340000 + i
        assert not any(self.depth.values()), self.depth
        return u.reg_read(reg.UC_ARM_REG_R0)

    def setup(self):
        u = self.u
        old = bytes(u.mem_read(PAF + 0x13344, 2))
        u.mem_write(PAF + 0x13344, b'\x70\xb5')
        before = bytes(u.mem_read(PAF + 0x50000, 0x2B0D00))
        self.call('initialize_pool')
        assert bytes(u.mem_read(PAF + 0x50000, 0x2B0D00)) == before
        u.mem_write(PAF + 0x13344, old)
        return self.read(self.sym['ready'])

    def shell_startup(self, shell, corrupt=False, fail_injection=-1):
        u = self.u
        fields = struct.unpack('<7I', u.mem_read(self.sym['profiles'] + self.firmware * 28, 28))
        nid, _, text_size, data_size, slot, init, request = fields
        assert len(shell) == text_size
        self.shell_base = base = 0x81800000
        data = 0x82000000
        u.mem_map(base, (text_size + 0xFFF) & ~0xFFF)
        u.mem_write(base, shell)
        u.mem_map(data, (data_size + 0xFFF) & ~0xFFF)
        info = HEAP + 0x6000
        word(u, info, 0x1B8)
        for i, (ptr, length) in enumerate(((base, text_size), (data, data_size))):
            word(u, info + 0x154 + 24*i + 8, ptr)
            word(u, info + 0x154 + 24*i + 12, length)
        # Loader relocation of the imported stack-guard address. Validate the
        # actual original opcode/register pair before rebasing its immediate.
        def relocated_mov(address, opcode, register, value):
            old_a, old_b = struct.unpack('<HH', u.mem_read(address, 4))
            assert old_a & ~0x040F == opcode and (old_b >> 8) & 15 == register
            a = opcode | (value >> 12) | ((value >> 1) & 0x400)
            b = ((value << 4) & 0x7000) | (register << 8) | (value & 255)
            u.mem_write(address, struct.pack('<HH', a, b))
        guard = self.sym['__stack_chk_guard']
        for offset, regno in ((init+6, 8), (request+8, 9)):
            relocated_mov(base+offset, 0xF240, regno, guard & 65535)
            relocated_mov(base+offset+4, 0xF2C0, regno, guard >> 16)
        if corrupt: u.mem_write(base + request, b'\x00\xbf')
        old_init = bytes(u.mem_read(base + init, 4))
        self.fail_injection = fail_injection
        result = self.call('instance_cache_start', 42, nid, info)
        if corrupt or fail_injection >= 0:
            assert result == 0xFFFFFFFF and not self.injections
            assert bytes(u.mem_read(base + init, 4)) == old_init
            assert self.call('instance_cache_can_unload') == 1
            return
        assert result == 0 and len(self.injections) == 2
        assert self.call('instance_cache_can_unload') == 0
        assert [a-base for a, _ in self.injections] == [init, request]
        # Supply the native pool result, then execute the installed four-byte
        # branch, C callback and explicit PUSH replay into the native resume.
        word(u, data + slot, ICON_POOL)
        word(u, self.sym['g_instance_pool_resume'], RETURN + 0x541)
        prefix = bytes(u.mem_read(PAF + 0x13344, 2))
        u.mem_write(PAF + 0x13344, b'\x70\xb5')
        self.call(base + init)
        u.mem_write(PAF + 0x13344, prefix)
        assert self.read(self.sym['ready']) == 1
        word(u, self.sym['g_instance_request_resume'], RETURN + 0x501)
        self.request_entry = base + request

    def image(self, resident=True):
        u = self.u
        u.mem_write(IMAGE, bytes(160))
        word(u, IMAGE, PAF + 0x2E1024)
        word(u, IMAGE + 4, CACHE)
        u.mem_write(IMAGE + 8, b'\x01\x00\x02\x00')
        word(u, IMAGE + 16, 2)
        word(u, IMAGE + 76, 1)
        u.mem_write(IMAGE + 81, b'\x01')
        word(u, IMAGE + 84, 10); word(u, IMAGE + 88, 10)
        word(u, IMAGE + 100, PAF + 0x2E106C)
        word(u, IMAGE + 156, SURFACE if resident else 0)
        word(u, SURFACE, SURFACE + 64)
        word(u, SURFACE + 68, RETURN + 0x121)
        word(u, SURFACE + 24, 1)
        self.expected_args = [HEAP + 0x4000, HANDLE_PTR, HEAP + 0x4100, 0x11223344, 0x55667788]
        assert self.call(getattr(self, 'request_entry', 'request_image'), *self.expected_args) == 0
        assert self.read(IMAGE) == self.sym['image_table'] + 8
        assert self.read(IMAGE + 100) == self.sym['image_table'] + 80
        assert self.read(self.sym['image_count']) == 1

    def widget(self):
        for offset in (252, 232, 236, 96):
            word(self.u, WVT + offset, RETURN + (0x141 if offset == 252 else 0x161))
        word(self.u, WIDGET, WVT)
        word(self.u, WIDGET + 356, HEAP + 0x5000)
        word(self.u, WIDGET + 588, OBJECT)
        word(self.u, WIDGET + 592, 1)
        word(self.u, OBJECT + 8, 0x800)
        word(self.u, WIDGET + 396, 1 << 28)
        word(self.u, HANDLE_PTR, IMAGE + 100)


def main():
    assert len(sys.argv) in (4, 5)
    plugin = elf(Path(sys.argv[1]))
    print(f'ARM branch encoder: {check_encoder(plugin)} cases passed')
    checks = 0
    shell_paths = os.environ.get('LIVEAREA_INSTANCE_SHELL_TEXTS', '').split(os.pathsep)
    if shell_paths == ['']: shell_paths = []
    assert not shell_paths or len(shell_paths) == len(sys.argv)-2
    for firmware, filename in enumerate(sys.argv[2:]):
        paf = Path(filename).read_bytes()
        if shell_paths:
            shell = Path(shell_paths[firmware]).read_bytes()
            for failure in (-1, 0, 1, 2):
                start = Harness(plugin, paf, firmware)
                start.shell_startup(shell, corrupt=failure == 2,
                                    fail_injection=failure if failure in (0, 1) else -1)
                if failure == -1: start.image()
                checks += 1
        h = Harness(plugin, paf, firmware)
        assert h.setup() == 1
        assert h.read(POOL) == h.sym['pool_table'] + 8
        assert h.read(h.sym['image_table'] + 72) == 0xFFFFFF9C
        h.call('initialize_pool')  # Real wrapper + PUSH replay + resume.
        h.image(); h.widget()
        assert h.call(h.read(h.read(POOL) + 8), POOL, 16, 32) == HEAP + 0x7000
        assert h.destroyed == 1 and h.allocations == 2 and h.free == 32
        assert h.read(IMAGE + 156) == 0
        assert h.call(PAF + 0x15F9E2, WIDGET, HANDLE_PTR, 0, 0) == 0xFFFFFFFF
        assert h.read(OBJECT + 8) == 0x800 and h.read(WIDGET + 396) == 1 << 28
        assert h.binds == 0 and h.gets == 1
        # Async completion, followed by allocation pressure in the exact gap.
        word(h.u, IMAGE + 156, SURFACE); word(h.u, SURFACE + 24, 1)
        h.u.mem_write(IMAGE + 10, b'\x02')
        h.pressure = True
        assert h.call(PAF + 0x15F9E2, WIDGET, HANDLE_PTR, 0, 0) is None
        assert h.read(SURFACE + 24) == 2
        context = h.u.context_save()
        saved_stack = bytes(h.u.mem_read(STACK, 0x10000))
        h.free = 0
        assert h.call(h.read(h.read(POOL) + 8), POOL, 16, 32) == 0
        assert h.read(IMAGE + 156) == SURFACE and h.destroyed == 1
        h.u.mem_write(STACK, saved_stack); h.u.context_restore(context)
        h.u.emu_start(PAF + 0x15FA13, RETURN, count=100000)
        assert h.u.reg_read(reg.UC_ARM_REG_PC) == RETURN
        assert h.u.reg_read(reg.UC_ARM_REG_R0) == 0 and h.binds == 1
        assert h.read(OBJECT + 8) == 0 and h.read(WIDGET + 396) == 0
        assert h.read(SURFACE + 24) == 2
        if 'transfers' in h.sym: assert h.read(h.sym['transfers']) == 1
        assert not any(h.depth.values())
        checks += 1
        # Validation failures must leave native tables/objects unmodified.
        for failure in ('nonrecursive', 'table', 'capacity', 'nid', 'consumer'):
            h = Harness(plugin, paf, firmware)
            if failure == 'nonrecursive': h.recursive = False
            if failure == 'table': word(h.u, PAF + 0x2E3968, 123)
            if failure == 'capacity': word(h.u, POOL + 12, 4096)
            if failure == 'nid': word(h.u, h.sym['profile'], h.sym['profiles'] + (0 if firmware == 1 else 1)*28)
            if failure == 'consumer': h.u.mem_write(PAF + 0x15FA10, b'\x00\xbf')
            assert h.setup() == 0
            assert h.read(POOL) == PAF + 0x2E3968
            assert not any(h.depth.values())
            checks += 1
    print(f'ARM instance backend: {checks} scenarios across {len(sys.argv)-2} firmware profiles passed; '
          'native allocation, native Apply pending/binding, reference pin across '
          'allocation pressure, five-argument entry ABI, recursive-lock and '
          'firmware rejection, unchanged PAF text/tables')


if __name__ == '__main__':
    main()
