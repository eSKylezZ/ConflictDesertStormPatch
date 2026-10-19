"""Builds and runs pse_tests.exe (tier 1 unit tests + tier 2 image tests) - no game is started.
See docs/sdk/testing.md.

  python scripts/run_tests.py                       # configure if needed, build Debug, run everything
  python scripts/run_tests.py "patch tables"        # only tests whose name contains the filter
  python scripts/run_tests.py --build-dir build-x --config RelWithDebInfo
  python scripts/run_tests.py --no-build            # run the last build
  python scripts/run_tests.py --live [-- pytest args]   # also run tier 3 (tests/live, needs a running game)

Exit code = number of failed tests (pse_tests) + 1 if the live tests failed; 100+ = build problem.
Environment: PSE_GAME_EXE = DesertStorm.exe to map for the image tests (default: Steam, then GOG install).
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CMAKE_DEFAULT = (r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions"
                 r"\Microsoft\CMake\CMake\bin\cmake.exe")


def find_cmake():
    if os.environ.get("CMAKE") and os.path.exists(os.environ["CMAKE"]):
        return os.environ["CMAKE"]
    if os.path.exists(CMAKE_DEFAULT):
        return CMAKE_DEFAULT
    return shutil.which("cmake")


def run(cmd, **kw):
    print("> " + " ".join(f'"{c}"' if " " in c else c for c in cmd), flush=True)
    return subprocess.call(cmd, **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("filter", nargs="?", help="substring of the test names to run")
    ap.add_argument("--build-dir", default="build-tests", help="CMake binary dir (default build-tests)")
    ap.add_argument("--config", default="Debug", help="Debug (default) / RelWithDebInfo / Release")
    ap.add_argument("--no-build", action="store_true", help="don't configure / build, run the existing exe")
    # ---- tier 3 (live tests against a running game; written separately in tests/live) ----
    ap.add_argument("--live", action="store_true",
                    help="LIVE: also run `python -m pytest tests/live` (needs the game running with the dev server)")
    ap.add_argument("pytest_args", nargs=argparse.REMAINDER, help="after --: extra arguments for pytest (--live)")
    a = ap.parse_args()

    build = a.build_dir if os.path.isabs(a.build_dir) else os.path.join(ROOT, a.build_dir)
    exe = os.path.join(build, "pse", a.config, "pse_tests.exe")
    if not a.no_build:
        cmake = find_cmake()
        if not cmake:
            print("cmake not found (set CMAKE=...)")
            return 101
        if not os.path.exists(os.path.join(build, "CMakeCache.txt")):
            if run([cmake, "-S", ROOT, "-B", build, "-G", "Visual Studio 17 2022", "-A", "Win32"]):
                return 102
        if run([cmake, "--build", build, "--config", a.config, "--target", "pse_tests"]):
            return 103
    if not os.path.exists(exe):
        print(f"{exe} not found - build first")
        return 104

    failures = run([exe] + ([a.filter] if a.filter else []), cwd=ROOT)

    if a.live:
        live = os.path.join(ROOT, "tests", "live")
        if os.path.isdir(live):
            extra = [x for x in a.pytest_args if x != "--"]
            if run([sys.executable, "-m", "pytest", live] + extra, cwd=ROOT):
                failures += 1
        else:
            print("--live: tests/live does not exist yet - skipped")
    print("result:", "OK" if failures == 0 else f"{failures} failure(s)")
    return failures


if __name__ == "__main__":
    sys.exit(main())
