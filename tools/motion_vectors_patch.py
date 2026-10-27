#!/usr/bin/env python3
# Writes shaders/patches/aa_motion_vectors.patch: upstream's deferred motion vectors (shaders/GTAIV.EFLC.FusionShaders-temp,
# snippets/MotionVectors.md) as a patch on the shader submodule, so that the build keeps assembling the submodule with
# our patches. The patch is the difference between upstream's in-repo copy and the submodule commit it was made from
# (BASE, an ancestor of our submodule commit). It sorts first and is skipped by buildshaders.bat once the submodule has
# it. After upstream changes the -temp copy: run this, then shaders/patches' generators and the rebase of the other
# patches on it.
#
# Usage: tools/motion_vectors_patch.py [base commit]; FUSION_SHADERS names another checkout of the submodule
import os
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SUBMODULE = Path(os.environ.get('FUSION_SHADERS', ROOT / 'shaders' / 'GTAIV.EFLC.FusionShaders'))
COPY = ROOT / 'shaders' / 'GTAIV.EFLC.FusionShaders-temp'
OUT = ROOT / 'shaders' / 'patches' / 'aa_motion_vectors.patch'
BASE = sys.argv[1] if len(sys.argv) > 1 else 'a8514056851f60b9332d772f37d536be9bc61883'


def git(*args, cwd, **kwargs):
    return subprocess.run(['git', *args], cwd=cwd, check=True, capture_output=True, **kwargs)


with tempfile.TemporaryDirectory() as temp:
    repo = Path(temp) / 'repo'
    repo.mkdir()
    # The base's files, committed in a scratch repository whose work tree then becomes upstream's copy
    archive = Path(temp) / 'base.tar'
    archive.write_bytes(git('archive', BASE, cwd=SUBMODULE).stdout)
    with tarfile.open(archive) as tar:
        tar.extractall(repo, filter='data')
    git('init', '-q', cwd=repo)
    git('add', '-A', cwd=repo)
    git('-c', 'user.name=x', '-c', 'user.email=x', 'commit', '-qm', 'base', cwd=repo)
    git('--work-tree', str(COPY), 'add', '-A', cwd=repo)
    patch = git('diff', '--cached', '--binary', cwd=repo).stdout

OUT.write_bytes(patch)
print(f'{OUT.relative_to(ROOT)}: {patch.count(b"diff --git ")} files')
