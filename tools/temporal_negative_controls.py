#!/usr/bin/env python3
"""Verify that the temporal tests reject broken ownership, readiness and wheel policies.

Run on a portable C++20 compiler, for example:
  python tools/temporal_negative_controls.py --compiler 'uv run --with ziglang python -m ziglang c++'
All builds and deliberate source mutations stay in a temporary directory.
"""
import argparse
import pathlib
import shlex
import shutil
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
CONTROLS = (
    ("readiness", "source/includes/frame_pacing.hpp", "generatedDue && gpuReady", "generatedDue"),
    ("ownership", "source/includes/sr_ring.hpp", "previous = {};", "/* previous frame incorrectly retained */"),
    ("wheel", "source/VehicleMotion.hpp", "float unit[2][3][3]{}, lengths[2][3]{};", "return false; float unit[2][3][3]{}, lengths[2][3]{};"),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="c++")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="temporal-controls-") as directory:
        stage = pathlib.Path(directory)
        (stage / "tests").mkdir()
        (stage / "source/includes").mkdir(parents=True)
        for name in ("main.cpp", "check.hpp", "frame_pacing_tests.cpp", "sr_ring_tests.cpp", "vehicle_motion_tests.cpp"):
            shutil.copyfile(ROOT / "tests" / name, stage / "tests" / name)
        for name in ("source/includes/upscaler_protocol.hpp", *(control[1] for control in CONTROLS)):
            shutil.copyfile(ROOT / name, stage / name)

        def run(name):
            binary = stage / name
            command = shlex.split(args.compiler) + ["-std=c++20", "-fshort-wchar", "-I" + str(stage / "source")]
            command += [str(path) for path in sorted((stage / "tests").glob("*.cpp"))]
            subprocess.run(command + ["-o", str(binary)], check=True)
            return subprocess.run([str(binary)], capture_output=True, text=True)

        baseline = run("baseline")
        if baseline.returncode:
            raise SystemExit("Baseline must pass before testing negative controls:\n" + baseline.stdout + baseline.stderr)
        print("PASS baseline")
        for name, filename, before, after in CONTROLS:
            path = stage / filename
            original = path.read_text()
            if original.count(before) != 1:
                raise SystemExit(f"Control {name} needs updating: mutation site is not unique")
            path.write_text(original.replace(before, after))
            try:
                result = run("broken-" + name)
                if result.returncode == 0:
                    raise SystemExit(f"FAIL {name}: tests accepted the broken policy")
                print("PASS rejected broken " + name)
            finally:
                path.write_text(original)


if __name__ == "__main__":
    main()
