#!/usr/bin/env python3
"""Line and branch coverage for the C core and the C-family bindings.

Needs a build configured with clang source-based coverage:

    cmake -S . -B build-cov -DCMAKE_BUILD_TYPE=Debug \\
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \\
        "-DCMAKE_C_FLAGS=-fprofile-instr-generate -fcoverage-mapping" \\
        "-DCMAKE_CXX_FLAGS=-fprofile-instr-generate -fcoverage-mapping" \\
        "-DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate" \\
        "-DCMAKE_SHARED_LINKER_FLAGS=-fprofile-instr-generate" \\
        "-DCMAKE_MODULE_LINKER_FLAGS=-fprofile-instr-generate"
    cmake --build build-cov -j

Each part of the code is measured only by the tests written in its own
language, so one suite can't make another look better than it is:

    core    src/                       the C tests (ctest minus _cpp/_py/interop)
    cpp     bindings/cpp/include/      the C++ binding test
    python  compress_utils_py.cpp      the Python tests
    fuzz    src/ (+ third_party total) fuzz_replay over --fuzz-dirs (nightly)

clang merges counters for the same function across every binary that ran, so
each group runs on its own and writes its own profile. Reports go to
<out>/<group>.txt and <out>/<group>-html/, and <out>/summary.md collects them
(CI appends it to the job summary). Exits 1 if any group's tests failed.

Usage:
    tools/coverage.py --build-dir build-cov [--out-dir coverage] [--groups core,cpp,python]
    tools/coverage.py --build-dir build-cov --groups fuzz --fuzz-dirs corpus/ seeds/
"""

import argparse
import glob
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

GROUPS = {
    "core": {
        "title": "C core (src/), C tests",
        "exclude": r"_cpp$|_py$|^test_interop",
        "sources": ["src"],
    },
    "cpp": {
        "title": "C++ binding (bindings/cpp/include/), C++ tests",
        "include": r"_cpp$",
        "sources": ["bindings/cpp/include"],
    },
    "python": {
        "title": "Python binding (compress_utils_py.cpp), Python tests",
        "include": r"_py$|^test_interop",
        "sources": ["bindings/python/compress_utils_py.cpp"],
    },
    "fuzz": {
        "title": "Fuzz corpus (src/), fuzz_replay",
        "sources": ["src"],
    },
}


def llvm_tool(name, build_dir):
    """The llvm-cov / llvm-profdata matching the build's compiler (the profile
    format changes between LLVM releases): LLVM_COV / LLVM_PROFDATA if set,
    else the tool beside the real compiler (e.g. /usr/lib/llvm-18/bin), else
    Xcode's for Apple clang, else the one on PATH."""
    var = name.upper().replace("-", "_")
    if os.environ.get(var):
        return [os.environ[var]]
    compiler = None
    with open(os.path.join(build_dir, "CMakeCache.txt")) as cache:
        for line in cache:
            if line.startswith("CMAKE_C_COMPILER:"):
                compiler = shutil.which(line.split("=", 1)[1].strip())
    if compiler:
        beside = os.path.join(os.path.dirname(os.path.realpath(compiler)), name)
        if os.access(beside, os.X_OK):
            return [beside]
        version = os.path.basename(compiler).partition("-")[2]  # clang-18 -> 18
        if version and shutil.which(f"{name}-{version}"):
            return [f"{name}-{version}"]
        apple = subprocess.run([compiler, "--version"], capture_output=True, text=True).stdout
        if apple.startswith("Apple") and shutil.which("xcrun"):
            return ["xcrun", name]
    if shutil.which(name):
        return [name]
    sys.exit(f"coverage.py: {name} not found; put it on PATH or set {var}")


def ctest_args(build_dir, group):
    args = ["ctest", "--test-dir", build_dir, "--output-on-failure"]
    if "include" in group:
        args += ["-R", group["include"]]
    if "exclude" in group:
        args += ["-E", group["exclude"]]
    return args


def ctest_commands(build_dir, group):
    """The command line of every test the group would run."""
    out = subprocess.run(ctest_args(build_dir, group) + ["--show-only=json-v1"],
                         check=True, capture_output=True, text=True).stdout
    return [t["command"] for t in json.loads(out).get("tests", []) if t.get("command")]


def python_module(python):
    """The extension module this build made, as the tests' Python imports it."""
    env = dict(os.environ, PYTHONPATH=os.path.join(ROOT, "bindings", "python"))
    return subprocess.run(
        [python, "-c", "import compress_utils.compress_utils_py as m; print(m.__file__)"],
        check=True, capture_output=True, text=True, env=env).stdout.strip()


def run_group(name, group, args, profile_dir):
    """Runs the group's tests, writing profiles to profile_dir. Returns
    (ok, binaries whose coverage mapping the report reads)."""
    env = dict(os.environ, LLVM_PROFILE_FILE=os.path.join(profile_dir, "%p-%m.profraw"))
    if name == "fuzz":
        replay = os.path.join(args.build_dir, "tests", "fuzz", "fuzz_replay")
        inputs = sorted(f for d in args.fuzz_dirs for f in glob.glob(os.path.join(d, "*"))
                        if os.path.isfile(f))
        if not inputs:
            sys.exit("coverage.py: --groups fuzz needs --fuzz-dirs with inputs in them")
        ok = True
        for i in range(0, len(inputs), 500):
            batch = subprocess.run([replay] + inputs[i:i + 500], env=env,
                                   stdout=subprocess.DEVNULL)
            ok &= batch.returncode == 0
        print(f"coverage.py: replayed {len(inputs)} fuzz inputs")
        return ok, [replay]

    commands = ctest_commands(args.build_dir, group)
    if not commands:
        print(f"coverage.py: no tests in group {name}; skipping")
        return True, []
    ok = subprocess.run(ctest_args(args.build_dir, group), env=env).returncode == 0
    if name == "python":
        # Python tests run as `<python> <test>.py`: the interpreter CMake found.
        return ok, [python_module(commands[0][0])]
    return ok, sorted({c[0] for c in commands})


def report(name, group, binaries, profdata, args, llvm_cov):
    objects = [binaries[0]] + [a for b in binaries[1:] for a in ("-object", b)]
    sources = [os.path.join(ROOT, s) for s in group["sources"]]
    base = llvm_cov + ["report"] + objects + ["-instr-profile", profdata]
    text = subprocess.run(base + sources, check=True, capture_output=True, text=True).stdout
    if name == "fuzz":
        # Which vendored decoder code the fuzzer reaches, as one total line.
        third = subprocess.run(base + [os.path.join(ROOT, "third_party")],
                               check=True, capture_output=True, text=True).stdout
        total = [l for l in third.splitlines() if l.startswith("TOTAL")]
        text += ("\nthird_party/ (encoders included, never fuzzed):\n"
                 + (total[0] if total else "TOTAL: no data") + "\n")
    text = text.replace(ROOT + os.sep, "")
    html_dir = os.path.join(args.out_dir, f"{name}-html")
    # The fuzz HTML includes third_party/: which decoder paths are never reached
    # (its total also counts the encoders, which the fuzzer never runs).
    html_sources = sources + ([os.path.join(ROOT, "third_party")] if name == "fuzz" else [])
    subprocess.run(llvm_cov + ["show"] + objects + ["-instr-profile", profdata, "-format=html",
                   "-show-branches=count", "-output-dir", html_dir] + html_sources, check=True)
    with open(os.path.join(args.out_dir, f"{name}.txt"), "w") as f:
        f.write(text)
    return f"### {group['title']}\n\n```\n{text.rstrip()}\n```\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--out-dir", default="coverage")
    parser.add_argument("--groups", default="core,cpp,python")
    parser.add_argument("--fuzz-dirs", nargs="*", default=[])
    args = parser.parse_args()
    args.build_dir = os.path.abspath(args.build_dir)
    args.out_dir = os.path.abspath(args.out_dir)
    llvm_cov = llvm_tool("llvm-cov", args.build_dir)
    llvm_profdata = llvm_tool("llvm-profdata", args.build_dir)

    os.makedirs(args.out_dir, exist_ok=True)
    summary, failed = [], []
    for name in args.groups.split(","):
        if name not in GROUPS:
            sys.exit(f"coverage.py: unknown group {name!r} (one of {', '.join(GROUPS)})")
        group = GROUPS[name]
        profile_dir = os.path.join(args.out_dir, "profiles", name)
        shutil.rmtree(profile_dir, ignore_errors=True)
        os.makedirs(profile_dir)
        ok, binaries = run_group(name, group, args, profile_dir)
        if not ok:
            failed.append(name)
        raw = glob.glob(os.path.join(profile_dir, "*.profraw"))
        if not binaries or not raw:
            continue
        profdata = os.path.join(args.out_dir, f"{name}.profdata")
        subprocess.run(llvm_profdata + ["merge", "-sparse", "-o", profdata] + raw, check=True)
        section = report(name, group, binaries, profdata, args, llvm_cov)
        print(section)
        summary.append(section)

    with open(os.path.join(args.out_dir, "summary.md"), "w") as f:
        f.write("\n".join(summary))
    if failed:
        print(f"coverage.py: tests failed in: {', '.join(failed)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
