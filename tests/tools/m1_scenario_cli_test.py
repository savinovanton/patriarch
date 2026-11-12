"""Check M1 golden hashes, replay, save continuation and content validation through simcli."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


exe, mapgen, data, start_hash, run_hash = sys.argv[1:]
seed = 7
run_days = 45
save_days = 17


def run(*args, data_dir=data, exit_code=0):
    command = [exe, "--scenario", "m1", "--data", str(data_dir), "--hash-only", *map(str, args)]
    result = subprocess.run(command, encoding="utf-8", errors="replace", capture_output=True, timeout=30)
    assert result.returncode == exit_code, (args, result.returncode, result.stdout, result.stderr)
    return result


def expect_hash(expected, *args):
    actual = run(*args).stdout.strip()
    assert actual == expected, (args, expected, actual)


with tempfile.TemporaryDirectory(prefix="patriarch-m1-") as directory:
    root = Path(directory)
    replay = root / "m1.prp"
    save = root / "m1.psv"
    map_file = root / "m1.pmd"

    expect_hash(start_hash, "--seed", seed, "--days", 0)
    subprocess.run(
        [mapgen, "--out", str(map_file), "--width", "256", "--height", "128",
         "--seed", "7", "--provinces", "20", "--threads", "1"],
        check=True, capture_output=True, timeout=30,
    )
    expect_hash(start_hash, "--map", map_file, "--seed", seed, "--days", 0)
    expect_hash(run_hash, "--seed", seed, "--days", run_days, "--record", replay)
    expect_hash(run_hash, "--replay", replay)
    expect_hash(run_hash, "--seed", seed, "--days", run_days, "--verify-determinism", "--quiet")
    saved_hash = run("--seed", seed, "--days", save_days, "--save", save).stdout.strip()
    expect_hash(run_hash, "--load", save, "--days", run_days - save_days)
    demo_load = [exe, "--load", str(save), "--hash-only"]
    inspect = subprocess.run(
        [*demo_load, "--days", "0", "--speed", "5"],
        encoding="utf-8", errors="replace", capture_output=True, timeout=30,
    )
    assert inspect.returncode == 0 and inspect.stdout.strip() == saved_hash, inspect
    wrong_scenario = subprocess.run(
        [*demo_load, "--days", "1"], encoding="utf-8", errors="replace", capture_output=True, timeout=30,
    )
    assert wrong_scenario.returncode == 4, wrong_scenario
    assert "--scenario m1" in wrong_scenario.stderr, wrong_scenario.stderr

    changed_data = root / "data"
    shutil.copytree(data, changed_data)
    provinces = changed_data / "provinces" / "m1.pd"
    original = provinces.read_text(encoding="utf-8")

    # Keep the same definition count and change one key so neither set is a subset of the other.
    last_province = 'province "p0020"'
    assert original.count(last_province) == 1
    provinces.write_text(original.replace(last_province, 'province "p0021"'), encoding="utf-8")
    holdings = changed_data / "scenarios" / "m1" / "holdings.pd"
    original_holdings = holdings.read_text(encoding="utf-8")
    holdings.write_text(original_holdings.replace("province:p0020", "province:p0021"), encoding="utf-8")
    invalid_map = run("--seed", seed, "--days", 0, data_dir=changed_data, exit_code=1)
    assert "map province 'p0020' has no content definition" in invalid_map.stderr, invalid_map.stderr
    assert "province definition 'p0021' has no map province" in invalid_map.stderr, invalid_map.stderr

    # Start with a valid change, then require loading the old save to fail on the content hash.
    provinces.write_text(original, encoding="utf-8")
    old_control = "control = 80"
    assert old_control in original_holdings
    holdings.write_text(original_holdings.replace(old_control, "control = 79", 1), encoding="utf-8")
    changed = run("--seed", seed, "--days", 0, data_dir=changed_data)
    assert changed.stdout.strip() != start_hash, changed.stdout
    stale_save = run("--load", save, "--days", 0, data_dir=changed_data, exit_code=4)
    assert "content hash" in stale_save.stderr, stale_save.stderr
    stale_replay = run("--replay", replay, data_dir=changed_data, exit_code=1)
    assert "different scenario or M1 content" in stale_replay.stderr, stale_replay.stderr
