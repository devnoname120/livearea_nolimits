from pathlib import Path
source=(Path(__file__).resolve().parents[1]/'src/recovery.c').read_text()
for required in ('SHELL_RECOVERY_LOAD_CALL_OFFSET 0xC1EU', 'shell_detour_encode_call(',
                 'taiInjectData(shell_modid,0,SHELL_RECOVERY_LOAD_CALL_OFFSET',
                 'module_acquire(&preload', 'prepare_recovery_module',
                 'taiInjectData(module.modid, 0, RECOVERY_STOP_OFFSET',
                 'original_finish(plugin)', 'module_release(&preload)'):
    assert required in source,required
for forbidden in ('taiHookFunctionImport(', 'taiHookFunctionOffset(', 'TAI_CONTINUE(',
                  'recovery_ready_hook', '"SceLibKernel"'):
    assert forbidden not in source,forbidden
