from pathlib import Path

source = (Path(__file__).resolve().parents[1] / "src" / "recovery.c").read_text()

for required in (
    "SHELL_RECOVERY_READY_OFFSET 0x1BFEU",
    "RECOVERY_STOP_OFFSET 0x1EU",
    "taiHookFunctionOffset(&ready_hook_ref, shell_modid",
    "taiInjectData(module.modid, 0, RECOVERY_STOP_OFFSET",
    "stop_redirect_prefix",
    "recovery_module_stop_redirect",
    'taiGetModuleInfo("SceDbRecovery"',
):
    assert required in source, required

for forbidden in (
    "taiHookFunctionImport(",
    '"SceLibKernel"',
    '"ScePaf"',
    "0x72CD301FU",
    "0x086867A8U",
    "0x8E4A7716U",
):
    assert forbidden not in source, forbidden
