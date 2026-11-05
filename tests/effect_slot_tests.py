#!/usr/bin/env python3
"""Keep SSR.fx's pixel shaders inside the ps_3_0 budget the game's device accepts.

D3DX compiles this effect while the world loads, and a pixel shader the device will not
create leaves the whole effect null: no reflections, no indirect light, no contact shadows,
no skin scattering, and a black screen. AO.fx at about 1330 slots did exactly that, while
SSR_PS at about 984 ran, so 984 is the ceiling this check holds the effect to.

Each entry also carries the size it was last reviewed at, and growing past that by more than
SLACK fails: with the march 4 slots under the ceiling, a growth has to be a decision rather
than a surprise. Update the baseline in ENTRIES once the new size has been seen in the game.

Needs fxc. On Windows source/dxsdk/lib/x86/fxc.exe runs directly; elsewhere the docker image
ff-wine (wine plus the SDK's fxc, with the native d3dcompiler_43, whose checks are stricter
than wine's built-in one). Without either, the check says it skipped and passes. Wine starts
for every entry, so the default list holds the heavy ones; --all walks the whole effect.
"""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FXC = ROOT / 'source/dxsdk/lib/x86/fxc.exe'
EFFECT = ROOT / 'source/resources/SSR.fx'

# The defines the game compiles the effect with (PostFxResource::nSSRSteps and the rest).
DEFINES = ('/DNUM_STEPS=32', '/DNUM_REFINE_STEPS=8', '/DGI_RAYS=4', '/DGI_STEPS=8')

HARD_LIMIT = 984  # slots: the largest a device in the field has been seen to accept
SLACK = 8         # slots an entry may grow past its reviewed size before this fails

# Entry point -> instruction slots when last reviewed, 2026-11-05, NUM_STEPS=32.
ENTRIES = {
    'SSRTrace_PS': 980,
    'SSRWater_PS': 855,
    'SSRFallback_PS': 513,
    'SSRTemporal_PS': 390,
    'SSGI_PS': 378,
    'SSRDebug_PS': 317,
    'SSRDenoise_PS': 288,
}
LIGHT_ENTRIES = {
    'ContactShadows_PS': 223,
    'GIContactTemporal_PS': 231,
    'SkinScatterFinal_PS': 174,
    'SSRSpread_PS': 170,
    'SkinScatter_PS': 167,
    'ContactTemporal_PS': 147,
    'SSR_PS': 138,
    'GIContactUpsample_PS': 130,
    'GIContactDenoise_PS': 120,
    'GIUpsample_PS': 97,
    'SSRUpsample_PS': 109,
    'ContactUpsample_PS': 84,
    'SkinDebug_PS': 28,
    'SkinLight_PS': 27,
    'MarchDepth_PS': 26,
    'HiZDepth_PS': 22,
    'SSRDebugCopy_PS': 5,
}

SLOTS = re.compile(r'approximately (\d+) instruction slots')


def docker_fxc():
    """The ff-wine command prefix, or None where docker or the image is missing."""
    if not shutil.which('docker'):
        return None
    inspect = subprocess.run(['docker', 'image', 'inspect', 'ff-wine'],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if inspect.returncode != 0:
        return None
    return ['docker', 'run', '--rm', '-v', f'{ROOT}:/src', '-e', 'WINEDLLOVERRIDES=d3dcompiler_43=n']


def compile_entry(entry, scratch, prefix):
    """Compile one entry point, returning its slot count or None, and its fxc output."""
    assembly = Path(scratch) / f'{entry}.asm'
    if prefix is None:
        command = [str(FXC), '/nologo', *DEFINES, '/T', 'ps_3_0', '/E', entry,
                   '/Fc', str(assembly), '/Fo', str(Path(scratch) / f'{entry}.pso'), str(EFFECT)]
    else:
        command = [*prefix, '-v', f'{scratch}:/s', 'ff-wine', 'wine', '/src/source/dxsdk/lib/x86/fxc.exe',
                   '/nologo', *DEFINES, '/T', 'ps_3_0', '/E', entry,
                   '/Fc', f'Z:\\s\\{entry}.asm', '/Fo', f'Z:\\s\\{entry}.pso',
                   'Z:\\src\\source\\resources\\SSR.fx']
    ran = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    match = SLOTS.search(assembly.read_text()) if assembly.exists() else None
    return (int(match.group(1)) if match else None), ran.stdout


def main():
    entries = dict(ENTRIES)
    if '--all' in sys.argv:
        entries.update(LIGHT_ENTRIES)
    # --only SSRTrace_PS,SSRWater_PS: wine starts once per entry, so a full walk is slow.
    only = [a.split('=', 1)[1] for a in sys.argv if a.startswith('--only=')]
    if only:
        wanted = {name for group in only for name in group.split(',')}
        entries = {k: v for k, v in entries.items() if k in wanted}
        unknown = wanted - set(entries)
        if unknown:
            print(f'no such entry: {", ".join(sorted(unknown))}')
            return 2
        if not entries:
            print('nothing to check, --all walks the light entries too')
            return 2
    if not EFFECT.exists():
        print(f'no {EFFECT.name} here, nothing to check')
        return 0
    prefix = None
    if not (sys.platform == 'win32' and FXC.exists()):
        prefix = docker_fxc()
        if prefix is None:
            print('skipped: no fxc.exe and no docker image ff-wine to run it in')
            return 0
    failures = 0
    # wine in docker writes as root, which the cleanup would otherwise trip over.
    with tempfile.TemporaryDirectory(prefix='effect-slots-', ignore_cleanup_errors=True) as scratch:
        for entry, reviewed in entries.items():
            slots, output = compile_entry(entry, scratch, prefix)
            if slots is None:
                print(f'{entry}: did not compile')
                for line in output.splitlines():
                    if 'error' in line.lower():
                        print(f'  {line.strip()}')
                failures += 1
                continue
            over = slots > HARD_LIMIT
            grown = slots > reviewed + SLACK
            state = 'OVER THE DEVICE BUDGET' if over else 'grew' if grown else 'ok'
            print(f'{entry:<22} {slots:>5} slots, reviewed at {reviewed:>4}  {state}')
            if over or grown:
                failures += 1
    if failures:
        print(f'{failures} of {len(entries)} entries outside their budget: the effect the game '
              f'loads is null when one of them is, which takes the reflections, the indirect '
              f'light, the contact shadows and the skin scattering with it')
        return 1
    print(f'{len(entries)} entries inside the {HARD_LIMIT} slot budget')
    return 0


if __name__ == '__main__':
    sys.exit(main())
