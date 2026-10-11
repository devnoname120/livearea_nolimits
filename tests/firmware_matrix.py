"""Validate compiled patches and native cache/recovery against local firmware.

Firmware bytes stay outside the repository. OS, scheduling and rendering are
modeled by the ARM harnesses; this is not a physical-device test.
"""
from pathlib import Path
import argparse
import hashlib
import json
import struct

from unicorn import arm_const as reg
from firmware_elf import FirmwareElf
from arm_helpers import elf, word, STACK, RETURN
from test_recovery_firmware import elf_symbols
from test_shell_capacity import NativeShell, check_page_instructions
import test_instance_cache_arm as cache
from test_recovery_preload_arm import Trial

ROOT = Path(__file__).resolve().parents[1]


def modules_for(profile, firmware_root):
    folder = profile['firmware'] if profile['hardware'] == 'official' else profile['firmware'] + '0.011'
    paths = {'shell': 'vs0/vsh/shell/shell.elf', 'paf': 'vs0/sys/external/libpaf.elf',
             'recovery': 'vs0/vsh/common/dbrecovery_plugin.elf'}
    spec = dict(profile)
    spec['modules'] = {}
    for name, suffix in paths.items():
        module = FirmwareElf(firmware_root / profile['hardware'] / folder / 'fs_dec' / suffix)
        expected = profile['modules'][name]
        assert f'0x{module.nid:08X}' == expected['nid']
        assert hashlib.sha256(module.text).hexdigest() == expected['text_sha256']
        spec['modules'][name] = {**expected, 'path': str(module.path)}
    return spec


def capacity(spec, symbols):
    module = FirmwareElf(spec['modules']['shell']['path'])
    patched = bytearray(module.text)
    for patch in spec['patches']:
        offset, size = patch['offset'], patch['size']
        assert patched[offset:offset+size].hex() == patch['expected']
        replacement = symbols[patch['symbol']]
        assert len(replacement) == size
        patched[offset:offset+size] = replacement
    admission = next(p['offset'] for p in spec['patches'] if p['symbol'] == 'patch_top_level_admission')
    message = next(p['offset'] for p in spec['patches'] if p['symbol'] == 'patch_top_level_message')
    for text, pages, top, counted in ((module.text, 10, 100, 500), (bytes(patched), 50, 500, 4000)):
        native = NativeShell(text, module.text_base, admission, message, pages)
        check_page_instructions(native.cpu, module.text_base, spec['patches'], pages)
        for count in (0, 99, 100, 101, 499, 500, 501, 3999, 4000, 4001):
            native.check_admission(count, min(count, counted), top, counted)
            native.check_message(count, top, counted)


def cache_scenarios(plugin, spec, index):
    paf = FirmwareElf(spec['modules']['paf']['path']).text
    shell = FirmwareElf(spec['modules']['shell']['path']).text
    for failure in (-1, 0, 1, 2):
        h = cache.Harness(plugin, paf, index, spec)
        h.shell_startup(shell, corrupt=failure == 2,
                        fail_injection=failure if failure in (0, 1) else -1)
        if failure == -1:
            h.image()
    h = cache.Harness(plugin, paf, index, spec)
    assert h.setup() == 1
    h.image(); h.widget()
    assert h.call(h.read(h.read(cache.POOL)+8), cache.POOL, 16, 32) == cache.HEAP+0x7000
    assert h.destroyed == 1 and h.read(cache.IMAGE+156) == 0
    consumer = h.native('apply_consumer', 0x15f9e2)
    assert h.call(consumer, cache.WIDGET, cache.HANDLE_PTR, 0, 0) == 0xffffffff
    assert h.gets == 1 and h.binds == 0
    word(h.u, cache.IMAGE+156, cache.SURFACE); word(h.u, cache.SURFACE+24, 1)
    h.u.mem_write(cache.IMAGE+10, b'\x02'); h.pressure = True
    assert h.call(consumer, cache.WIDGET, cache.HANDLE_PTR, 0, 0) is None
    assert h.read(cache.SURFACE+24) == 2
    context = h.u.context_save(); stack = bytes(h.u.mem_read(STACK, 0x10000)); h.free = 0
    assert h.call(h.read(h.read(cache.POOL)+8), cache.POOL, 16, 32) == 0
    h.u.mem_write(STACK, stack); h.u.context_restore(context)
    h.u.emu_start(h.native('apply_status_return', 0x15fa12)|1, RETURN, count=100000)
    assert h.u.reg_read(reg.UC_ARM_REG_PC) == RETURN and h.binds == 1
    assert h.read(cache.OBJECT+8) == 0 and h.read(cache.WIDGET+396) == 0
    assert not any(h.depth.values())
    for fault in ('nid', 'capacity', 'table', 'consumer', 'mutex'):
        h = cache.Harness(plugin, paf, index, spec)
        if fault == 'nid': h.paf_nid = 0xdeadbeef
        if fault == 'capacity': word(h.u, cache.POOL+12, 4096)
        if fault == 'table': word(h.u, h.native('pool_table', 0x2e3960)+8, 123)
        if fault == 'consumer': h.u.mem_write(h.native('apply_status', 0x15fa08)+8, b'\x00\xbf')
        if fault == 'mutex': h.recursive = False
        assert h.setup() == 0
    return 10


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--firmware-root', type=Path, required=True)
    p.add_argument('--plugin', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--limit', type=int)
    args = p.parse_args()
    plugin, symbols = elf(args.plugin), elf_symbols(args.plugin)
    profiles = json.loads((ROOT/'docs/firmware-profiles.json').read_text())
    results = []
    for index, profile in enumerate(profiles[:args.limit]):
        spec = modules_for(profile, args.firmware_root)
        capacity(spec, symbols)
        checks = cache_scenarios(plugin, spec, index)
        for counts in ((500,49), (500,73)):
            Trial(plugin, ROOT/'build', index, counts, spec).execute()
        result = {'hardware': spec['hardware'], 'firmware': spec['firmware'],
                  'capacity': 'passed', 'cache_scenarios': checks,
                  'native_recovery_counts': [[500,49],[500,73]], 'device_tested': False}
        results.append(result)
        print(json.dumps(result), flush=True)
    args.output.write_text(json.dumps({'verified':results, 'device_tested':False}, indent=2)+'\n')


if __name__ == '__main__':
    main()
