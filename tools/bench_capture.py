"""Capture raw Google Benchmark iterations with a comparable, path-free machine/build profile."""
import argparse
import json
import math
import os
from pathlib import Path
import platform
import re
import subprocess
import sys

import bench_compare


def benchmark_cpu():
    """Keep benchmarks off CPU 0's usual UI/interrupt traffic; change no system affinity."""
    if sys.platform != "win32":
        return None
    import ctypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetCurrentProcess.restype = ctypes.c_void_p
    kernel.GetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t),
                                             ctypes.POINTER(ctypes.c_size_t)]
    kernel.GetProcessAffinityMask.restype = ctypes.c_int
    allowed, system = ctypes.c_size_t(), ctypes.c_size_t()
    if not kernel.GetProcessAffinityMask(kernel.GetCurrentProcess(), ctypes.byref(allowed), ctypes.byref(system)) or not allowed.value:
        raise ValueError("Cannot read the current allowed processor mask")
    return allowed.value.bit_length() - 1


def read_cache(build):
    result = {}
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        match = re.match(r"^([^#/:][^:]*):[^=]+=(.*)$", line)
        if match:
            result[match[1]] = match[2]
    return result


def compiler_profile(build, cache):
    version = ".".join(cache.get("CMAKE_CACHE_" + part + "_VERSION", "")
                       for part in ("MAJOR", "MINOR", "PATCH"))
    path = build / "CMakeFiles" / version / "CMakeCXXCompiler.cmake"
    if not path.is_file():
        files = list((build / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
        if len(files) != 1:
            raise ValueError("Cannot identify one current CMake compiler file; use a clean build directory")
        path = files[0]
    text = path.read_text(encoding="utf-8")
    result = {}
    for key, suffix in (("id", "ID"), ("version", "VERSION"), ("architecture_id", "ARCHITECTURE_ID")):
        match = re.search(r'^set\(CMAKE_CXX_COMPILER_' + suffix + r'\s+(?:"([A-Za-z0-9_.+ -]+)"|([A-Za-z0-9_.+-]+))\s*\)',
                          text, re.MULTILINE)
        if not match:
            raise ValueError("Missing literal CMake compiler metadata: " + suffix)
        result[key] = match[1] or match[2]
    return result


def normalize(raw, names, source_sha, profile, repetitions=None):
    factors = {"ns": 1, "us": 1000, "ms": 1_000_000, "s": 1_000_000_000}
    groups = {name: {"name": name, "time_unit": "ns", "samples": []} for name in names}
    seen = set()
    labels = {}
    for row in raw.get("benchmarks", []):
        if row.get("error_occurred"):
            raise ValueError("Benchmark reported an error")
        if row.get("run_type") != "iteration":
            continue
        name = row.get("run_name", row.get("name"))
        if name not in groups:
            raise ValueError("Unexpected benchmark: " + str(name))
        index = row.get("repetition_index")
        if not isinstance(index, int) or isinstance(index, bool) or index < 0:
            raise ValueError("Raw repetition_index is required")
        if (name, index) in seen:
            raise ValueError("Duplicate raw repetition")
        seen.add((name, index))
        unit = row.get("time_unit")
        value = row.get("real_time")
        if (unit not in factors or not isinstance(value, (int, float)) or isinstance(value, bool)
                or not math.isfinite(value) or value <= 0 or type(row.get("iterations")) is not int
                or row["iterations"] <= 0):
            raise ValueError("Invalid iteration timing or unit")
        item = groups[name]
        label = row.get("label")
        if name in labels and labels[name] != label:
            raise ValueError("Benchmark result label/presence changed within a run")
        labels[name] = label
        if label is not None:
            if not isinstance(label, str) or ("label" in item and item["label"] != label):
                raise ValueError("Benchmark result label changed within a run")
            item["label"] = label
        item["samples"].append(value * factors[unit])
    if any(not item["samples"] for item in groups.values()):
        raise ValueError("Missing raw benchmark iterations (aggregate-only output cannot be gated)")
    if repetitions is not None and any({index for group, index in seen if group == name} != set(range(repetitions))
                                       for name in names):
        raise ValueError("Raw repetitions must exactly cover 0..repetitions-1")
    return {"schema": 1, "source_sha": source_sha, "profile": profile,
            "benchmarks": list(groups.values())}


def capture(source, build, suite_path, destination):
    source, build, destination = source.resolve(), build.resolve(), destination.resolve()
    suite = bench_compare.validate_suite(bench_compare.read_json(suite_path))
    names = suite["benchmarks"]
    settings = suite["capture"]
    if settings["repetitions"] < suite["min_samples"]:
        raise ValueError("Capture repetitions are below the gate's minimum")
    cache = read_cache(build)
    if not cache.get("CMAKE_HOME_DIRECTORY") or Path(cache["CMAKE_HOME_DIRECTORY"]).resolve() != source:
        raise ValueError("Build directory belongs to a different source tree")
    if cache.get("PATRIARCH_SANITIZE_ADDRESS", "OFF").upper() not in ("OFF", "FALSE", "0", ""):
        raise ValueError("ASan measurements are not performance baselines")
    if cache.get("PATRIARCH_ENABLE_TRACY", "OFF").upper() not in ("OFF", "FALSE", "0", ""):
        raise ValueError("Tracy profiling must be disabled in performance baselines")
    if any("sanitize" in cache.get(key, "").lower()
           for key in ("CMAKE_CXX_FLAGS", "CMAKE_CXX_FLAGS_RELEASE")):
        raise ValueError("Sanitized compiler flags are not performance baselines")
    configurations = cache.get("CMAKE_CONFIGURATION_TYPES", "").split(";")
    if "Release" not in configurations and cache.get("CMAKE_BUILD_TYPE") != "Release":
        raise ValueError("A Release build is required")
    executable = build / "bench" / ("Release" if "Release" in configurations else "") / (
        "bench.exe" if sys.platform == "win32" else "bench")
    if not executable.is_file():
        raise ValueError("Release benchmark executable does not exist; build target bench first")
    source_sha = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if not re.fullmatch(r"[0-9a-f]{40}", source_sha):
        raise ValueError("Expected an exact source commit")
    # A code baseline must refer to committed workload/compiler inputs, not a dirty working tree.
    changed = subprocess.check_output(["git", "-C", str(source), "diff", "HEAD", "--", "bench",
        "src/core", "src/sim", "cmake", "CMakeLists.txt", "CMakePresets.json", "vcpkg.json"], text=True)
    if changed:
        raise ValueError("Benchmark inputs have tracked uncommitted changes; use a clean source worktree")
    destination.parent.mkdir(parents=True, exist_ok=True)
    raw_path = destination.with_name(destination.stem + ".raw.json")
    raw_path.unlink(missing_ok=True)
    destination.unlink(missing_ok=True)
    # Google Benchmark's POSIX regex does not accept Python's non-capturing (?:...) groups.
    pattern = "^(" + "|".join(re.escape(name) for name in names) + ")$"
    cpu = benchmark_cpu()
    priority = "above_normal" if sys.platform == "win32" else "normal"
    command = [str(executable), "--benchmark_filter=" + pattern,
               "--benchmark_min_time=" + str(settings["min_time_seconds"]) + "s",
               "--benchmark_min_warmup_time=" + str(settings["warmup_seconds"]),
               "--benchmark_repetitions=" + str(settings["repetitions"]),
               "--benchmark_out=" + str(raw_path), "--benchmark_out_format=json",
               "--benchmark_format=console"]
    if cpu is not None:
        command.append("--pat_cpu=" + str(cpu))
    with destination.with_suffix(".log").open("w", encoding="utf-8") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=600,
                       creationflags=getattr(subprocess, "ABOVE_NORMAL_PRIORITY_CLASS", 0))
    raw = bench_compare.read_json(raw_path)
    context = raw.get("context", {})
    if context.get("library_build_type", "").lower() != "release":
        raise ValueError("Google Benchmark library must be Release")
    affinity = str(context.get("cpu_affinity", ""))
    if not affinity.isdecimal():
        raise ValueError("A pinned allowed CPU is required")
    if cpu is not None and affinity != str(cpu):
        raise ValueError("Benchmark affinity differs from the requested allowed CPU")
    if not isinstance(context.get("library_version"), str) or not context["library_version"]:
        raise ValueError("Google Benchmark version is required")
    profile = {"protocol": "patriarch.bench.v1", "time_field": "real_time", "os": sys.platform,
               "architecture": platform.machine(), "cpu_model": platform.processor(),
               "cpu_count": os.cpu_count(), "cpu_affinity": affinity,
               "priority_class": priority,
               "caches": context.get("caches", []), "compiler": compiler_profile(build, cache),
               "configuration": "Release", "benchmark_version": context.get("library_version"),
               "benchmark_library_build_type": context["library_build_type"].lower()}
    result = normalize(raw, names, source_sha, profile, settings["repetitions"])
    bench_compare.validate_measurement(result, suite)
    # Keep timings exactly as emitted; omit only host names and local executable paths.
    context.pop("host_name", None)
    context.pop("executable", None)
    raw_path.write_text(json.dumps(raw, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary = destination.with_suffix(destination.suffix + ".tmp")
    temporary.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(destination)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--suite", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = capture(args.source, args.build_dir, args.suite, args.output)
        print("Captured", len(result["benchmarks"]), "benchmarks from", result["source_sha"])
    except (ValueError, KeyError, OSError, subprocess.SubprocessError) as error:
        print("bench_capture:", error, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
