"""Compare normalized Google Benchmark iteration samples; never gate on aggregate rows.

Exit codes: 0 pass, 1 stable regression, 2 invalid/incompatible input or I/O error,
3 inconclusive (noise or too few samples). No baseline is changed by compare.
"""

import argparse
from decimal import Decimal
import html
import json
import math
import os
from pathlib import Path
import re
import statistics
import sys
import tempfile


class InputError(ValueError):
    """A measurement cannot support a trustworthy comparison."""


_SHA = re.compile(r"^[0-9a-fA-F]{40}$")
_PRIVATE_PATH = re.compile(r"[a-zA-Z]:[\\/]|^\\\\|/(?:Users|home|private|tmp)/")
_PRIVATE_KEYS = {
    "host", "hostname", "computername", "user", "username", "executable",
    "path", "cwd", "home", "homedir", "sourcedir", "builddir",
}
_STATUSES = {
    "pass": "Пройдено",
    "regression": "Регрессия",
    "inconclusive": "Недостаточно достоверных данных",
    "invalid": "Несопоставимые или ошибочные данные",
}


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise InputError("duplicate JSON key: " + key)
        result[key] = value
    return result


def read_json(path):
    """Read strict JSON, rejecting duplicate keys and nonstandard NaN/Infinity."""
    path = Path(path)
    try:
        with path.open(encoding="utf-8-sig") as source:
            return json.load(source, object_pairs_hook=_object,
                             parse_constant=lambda value: _bad_constant(value))
    except OSError as error:
        raise InputError("{}: {}".format(path.name, error.strerror)) from error
    except (json.JSONDecodeError, UnicodeError) as error:
        raise InputError("{}: invalid UTF-8 JSON ({})".format(path.name, error)) from error


def _bad_constant(value):
    raise InputError("non-finite JSON constant: " + value)


def _number(value):
    try:
        return type(value) in (int, float) and math.isfinite(value)
    except OverflowError:
        return False


def validate_suite(document):
    if not isinstance(document, dict) or type(document.get("schema")) is not int or document["schema"] != 1:
        raise InputError("suite needs schema 1")
    names = document.get("benchmarks")
    if not isinstance(names, list) or not names:
        raise InputError("suite needs a nonempty benchmark list")
    if any(not isinstance(name, str) or not name or len(name) > 512 for name in names):
        raise InputError("suite benchmark names must be nonempty strings (<=512 characters)")
    if len(set(names)) != len(names):
        raise InputError("suite contains duplicate benchmark names")
    threshold = document.get("threshold")
    max_rsd = document.get("max_rsd")
    minimum = document.get("min_samples")
    if not _number(threshold) or not 0 <= threshold <= 1:
        raise InputError("suite threshold must be a finite fraction in [0,1]")
    if not _number(max_rsd) or not 0 < max_rsd <= 1:
        raise InputError("suite max_rsd must be a finite fraction in (0,1]")
    if type(minimum) is not int or not 2 <= minimum <= 10000:
        raise InputError("suite min_samples must be an integer in [2,10000]")
    require_same_label = document.get("require_same_label", False)
    if type(require_same_label) is not bool:
        raise InputError("suite require_same_label must be boolean")
    result = {"schema": 1, "benchmarks": list(names), "threshold": threshold,
              "max_rsd": max_rsd, "min_samples": minimum,
              "require_same_label": require_same_label}
    if "capture" in document:
        capture = document["capture"]
        if not isinstance(capture, dict) or set(capture) != {"repetitions", "min_time_seconds", "warmup_seconds"}:
            raise InputError("suite capture needs repetitions, min_time_seconds and warmup_seconds")
        if type(capture["repetitions"]) is not int or not minimum <= capture["repetitions"] <= 10000:
            raise InputError("suite capture repetitions must meet min_samples and be <=10000")
        if not _number(capture["min_time_seconds"]) or capture["min_time_seconds"] <= 0:
            raise InputError("suite capture min_time_seconds must be positive and finite")
        if not _number(capture["warmup_seconds"]) or capture["warmup_seconds"] < 0:
            raise InputError("suite capture warmup_seconds must be nonnegative and finite")
        result["capture"] = dict(capture)
    return result


def _profile(value, location="profile"):
    if isinstance(value, dict):
        if not value:
            raise InputError(location + " must not be empty")
        for key, item in value.items():
            if not isinstance(key, str) or not key:
                raise InputError(location + " has an invalid key")
            normalized = re.sub(r"[^a-z0-9]", "", key.lower())
            if normalized in _PRIVATE_KEYS or normalized.endswith("path"):
                raise InputError(location + "." + key + " is private/path metadata")
            _profile(item, location + "." + key)
    elif isinstance(value, list):
        for index, item in enumerate(value):
            _profile(item, location + "[{}]".format(index))
    elif isinstance(value, str):
        if _PRIVATE_PATH.search(value):
            raise InputError(location + " contains a filesystem path")
    elif value is None or type(value) is bool:
        return
    elif not _number(value):
        raise InputError(location + " is not a finite JSON value")


def validate_measurement(document, suite):
    if not isinstance(document, dict) or type(document.get("schema")) is not int or document["schema"] != 1:
        raise InputError("measurement needs normalized schema 1")
    revision = document.get("source_sha")
    if not isinstance(revision, str) or not _SHA.fullmatch(revision):
        raise InputError("measurement source_sha must be a 40-character Git revision")
    profile = document.get("profile")
    if not isinstance(profile, dict) or not profile:
        raise InputError("measurement needs a nonempty comparable profile object")
    _profile(profile)
    if profile.get("configuration") not in ("Release", "RelWithDebInfo"):
        raise InputError("measurement configuration must be Release or RelWithDebInfo")
    library_build = profile.get("benchmark_library_build_type")
    if library_build is not None and (not isinstance(library_build, str) or library_build.lower() != "release"):
        raise InputError("Google Benchmark library must use a release build")
    rows = document.get("benchmarks")
    if not isinstance(rows, list) or not rows:
        raise InputError("measurement needs normalized benchmark samples")
    indexed = {}
    for row in rows:
        if not isinstance(row, dict):
            raise InputError("benchmark entry must be an object")
        name = row.get("name")
        if not isinstance(name, str) or not name:
            raise InputError("benchmark entry needs a name")
        if name in indexed:
            raise InputError("duplicate benchmark: " + name)
        if row.get("time_unit") != "ns":
            raise InputError(name + ": normalized time_unit must be ns")
        samples = row.get("samples")
        if not isinstance(samples, list) or not samples:
            raise InputError(name + ": raw iteration samples are required (aggregate-only input is invalid)")
        if len(samples) > 10000 or any(not _number(value) or value <= 0 for value in samples):
            raise InputError(name + ": samples must be positive finite numbers (at most 10000)")
        label = row.get("label")
        if label is not None and (not isinstance(label, str) or len(label) > 4096):
            raise InputError(name + ": result label must be a string or absent")
        if label is not None and _PRIVATE_PATH.search(label):
            raise InputError(name + ": result label contains a filesystem path")
        normalized = {"name": name, "time_unit": "ns", "samples": list(samples)}
        if label is not None:
            normalized["label"] = label
        indexed[name] = normalized
    expected = set(suite["benchmarks"])
    missing, unexpected = expected - set(indexed), set(indexed) - expected
    if missing or unexpected:
        raise InputError("benchmark set mismatch; missing [{}], unexpected [{}]".format(
            ", ".join(sorted(missing)), ", ".join(sorted(unexpected))))
    # Rebuild the document: raw host names, command paths and unknown metadata cannot be frozen.
    return {"schema": 1, "source_sha": revision.lower(), "profile": profile,
            "benchmarks": [indexed[name] for name in suite["benchmarks"]]}


def summarize(samples):
    ordered = sorted(samples)
    middle = len(ordered) // 2
    median = ordered[middle] if len(ordered) % 2 else ordered[middle - 1] + (
        ordered[middle] - ordered[middle - 1]) / 2
    maximum = ordered[-1]
    scaled = [value / maximum for value in ordered]
    mean_scaled = statistics.mean(scaled)
    rsd = statistics.stdev(scaled) / mean_scaled if len(scaled) > 1 else None
    return {"samples": len(ordered), "median_ns": median,
            "mean_ns": mean_scaled * maximum, "rsd": rsd}


def _canonical(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=False, allow_nan=False, separators=(",", ":"))


def _above_threshold(baseline_median, candidate_median, threshold):
    baseline = Decimal(str(baseline_median))
    candidate = Decimal(str(candidate_median))
    limit = baseline * (1 + Decimal(str(threshold)))
    # Protect a few binary64 rounding steps (e.g. 100 * 1.1), not real slowdowns.
    epsilon = max(abs(limit), abs(candidate)) * Decimal(str(sys.float_info.epsilon)) * 8
    return candidate - limit > epsilon


def compare_measurements(baseline, candidate, suite):
    """Return a self-contained report; malformed/incompatible inputs raise InputError."""
    suite = validate_suite(suite)
    baseline = validate_measurement(baseline, suite)
    candidate = validate_measurement(candidate, suite)
    if _canonical(baseline["profile"]) != _canonical(candidate["profile"]):
        raise InputError("baseline/candidate profiles differ; use measurements from the same comparable environment")
    report = {"schema": 1, "status": "pass", "exit_code": 0,
              "baseline_sha": baseline["source_sha"], "candidate_sha": candidate["source_sha"],
              "profile": baseline["profile"], "policy": suite, "benchmarks": [], "errors": [], "warnings": []}
    regressions = inconclusive = 0
    for before, after in zip(baseline["benchmarks"], candidate["benchmarks"]):
        name = before["name"]
        label_changed = before.get("label") != after.get("label")
        if label_changed and suite["require_same_label"]:
            raise InputError(name + ": result labels differ; workload results are not comparable")
        if label_changed:
            report["warnings"].append(name + ": result label changed; review the functional/golden change")
        first, second = summarize(before["samples"]), summarize(after["samples"])
        change_decimal = Decimal(str(second["median_ns"])) / Decimal(str(first["median_ns"])) - 1
        change = float(change_decimal)
        if not math.isfinite(change):
            raise InputError(name + ": median ratio cannot be represented as a finite number")
        row = {"name": name, "baseline": first, "candidate": second,
               "change_fraction": change, "label_changed": label_changed,
               "status": "pass", "reason": "within threshold"}
        if min(first["samples"], second["samples"]) < suite["min_samples"]:
            row.update(status="inconclusive", reason="insufficient iteration samples")
        elif first["rsd"] > suite["max_rsd"] or second["rsd"] > suite["max_rsd"]:
            row.update(status="inconclusive", reason="RSD exceeds the noise limit; not classified as regression")
        elif _above_threshold(first["median_ns"], second["median_ns"], suite["threshold"]):
            row.update(status="regression", reason="stable median exceeds the threshold")
        regressions += row["status"] == "regression"
        inconclusive += row["status"] == "inconclusive"
        report["benchmarks"].append(row)
    report["summary"] = {"total": len(report["benchmarks"]), "regressions": regressions,
                         "inconclusive": inconclusive,
                         "passed": len(report["benchmarks"]) - regressions - inconclusive}
    if regressions:
        report.update(status="regression", exit_code=1)
    elif inconclusive:
        report.update(status="inconclusive", exit_code=3)
    return report


def freeze_measurement(measurement, suite):
    suite = validate_suite(suite)
    normalized = validate_measurement(measurement, suite)
    report = compare_measurements(normalized, normalized, suite)
    if report["exit_code"]:
        raise InputError("cannot freeze insufficient/noisy measurements; the existing reference was not changed")
    return normalized


def write_json_atomic(path, document):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n",
                                         dir=str(path.parent), prefix=path.name + ".", suffix=".tmp",
                                         delete=False) as target:
            temporary = Path(target.name)
            json.dump(document, target, indent=2, ensure_ascii=False, allow_nan=False)
            target.write("\n")
        os.replace(str(temporary), str(path))
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def render_html(report):
    """A portable artifact: inline SVG, no JavaScript, network or plotting dependency."""
    escape = lambda value: html.escape(str(value), quote=True)
    rows = report.get("benchmarks", [])
    maximum = max((max(row["baseline"]["median_ns"], row["candidate"]["median_ns"]) for row in rows), default=1)
    chart = ['<svg xmlns="http://www.w3.org/2000/svg" width="1100" height="{}" viewBox="0 0 1100 {}" role="img" aria-label="Медианы времени бенчмарков">'.format(65 * len(rows) + 40, 65 * len(rows) + 40)]
    table = []
    for index, row in enumerate(rows):
        y = index * 65 + 25
        name = escape(row["name"])
        before, after = row["baseline"], row["candidate"]
        color = {"pass": "#c8791e", "regression": "#c23b3b", "inconclusive": "#777"}[row["status"]]
        chart.append('<text x="8" y="{}" font-size="12">{}</text>'.format(y + 15, name))
        for offset, sample, fill in ((0, before, "#2777b2"), (23, after, color)):
            width = max(1, sample["median_ns"] / maximum * 570)
            chart.append('<rect x="365" y="{}" width="{:.3f}" height="18" fill="{}"><title>{}: {:.4g} ns</title></rect>'.format(y + offset, width, fill, name, sample["median_ns"]))
        change = row["change_fraction"] * 100
        chart.append('<text x="945" y="{}" font-size="12">{:+.2f}%</text>'.format(y + 23, change))
        def rsd_text(summary):
            return "—" if summary["rsd"] is None else "{:.2f}%".format(summary["rsd"] * 100)
        table.append("<tr><td>{}</td><td>{:.5g}</td><td>{:.5g}</td><td>{:+.2f}%</td><td>{} / {}</td><td>{} / {}</td><td>{}</td><td>{}</td></tr>".format(
            name, before["median_ns"], after["median_ns"], change,
            before["samples"], after["samples"], rsd_text(before), rsd_text(after),
            escape(_STATUSES[row["status"]]), escape(row["reason"])))
    chart.append("</svg>")
    errors = "".join("<li>{}</li>".format(escape(message)) for message in report.get("errors", []))
    warnings = "".join("<li>{}</li>".format(escape(message)) for message in report.get("warnings", []))
    profile = escape(json.dumps(report.get("profile", {}), ensure_ascii=False, sort_keys=True, indent=2))
    policy = report.get("policy", {})
    return """<!doctype html><html lang="ru"><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>Сравнение производительности</title>
<style>body{{font:16px system-ui,sans-serif;max-width:1200px;margin:2rem auto;padding:0 1rem;color:#222}}table{{border-collapse:collapse;width:100%;font-size:14px}}th,td{{padding:.55rem;text-align:left;border-bottom:1px solid #ccc}}pre{{white-space:pre-wrap;background:#f4f4f4;padding:1rem}}.chart{{overflow:auto}}svg{{background:#fafafa}}code{{overflow-wrap:anywhere}}</style>
<h1>Сравнение производительности</h1><p><strong>{status}</strong> · exit code {code}</p>
<p>Эталон: <code>{base}</code><br>Кандидат: <code>{candidate}</code></p>
<p>Порог: {threshold}%; максимум RSD: {rsd}%; минимум повторов: {minimum}.
Синие столбцы — эталон, вторые — кандидат. Квантили агрегатных строк не используются.</p>
<ul>{errors}</ul><ul>{warnings}</ul><div class="chart">{chart}</div>
<table><thead><tr><th>Бенчмарк</th><th>Медиана эталона, ns</th><th>Медиана кандидата, ns</th><th>Изменение</th><th>Повторы</th><th>RSD эталон / кандидат</th><th>Итог</th><th>Причина</th></tr></thead><tbody>{table}</tbody></table>
<h2>Профиль измерения</h2><pre>{profile}</pre>
<p>Нестабильные и неполные измерения не объявляются регрессией. При неопределённости без доказанных регрессий результат — exit code 3, а не успешная проверка. Обновление эталона выполняется отдельной командой freeze.</p>
</html>""".format(status=escape(_STATUSES.get(report["status"], report["status"])), code=report["exit_code"],
                 base=escape(report.get("baseline_sha", "—")), candidate=escape(report.get("candidate_sha", "—")),
                 threshold=escape(policy.get("threshold", 0) * 100), rsd=escape(policy.get("max_rsd", 0) * 100),
                 minimum=escape(policy.get("min_samples", "—")), errors=errors, warnings=warnings,
                 chart="".join(chart), table="".join(table), profile=profile)


def _distinct_outputs(outputs, inputs):
    normalized = [os.path.normcase(str(Path(path).resolve())) for path in outputs]
    sources = {os.path.normcase(str(Path(path).resolve())) for path in inputs}
    if len(set(normalized)) != len(normalized) or any(path in sources for path in normalized):
        raise InputError("comparison outputs must be distinct and must not overwrite inputs/policy")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    comparison = commands.add_parser("compare", help="compare without modifying the reference")
    comparison.add_argument("baseline", type=Path)
    comparison.add_argument("candidate", type=Path)
    comparison.add_argument("--suite", type=Path, required=True)
    comparison.add_argument("--json", type=Path, required=True)
    comparison.add_argument("--html", type=Path, required=True)
    freeze = commands.add_parser("freeze", help="explicitly validate and atomically update a reference")
    freeze.add_argument("input", type=Path)
    freeze.add_argument("--suite", type=Path, required=True)
    freeze.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "freeze":
            if os.path.normcase(str(args.output.resolve())) == os.path.normcase(str(args.suite.resolve())):
                raise InputError("freeze must not overwrite suite policy")
            normalized = freeze_measurement(read_json(args.input), read_json(args.suite))
            write_json_atomic(args.output, normalized)
            print("reference updated explicitly: " + args.output.name)
            return 0
        _distinct_outputs([args.json, args.html], [args.baseline, args.candidate, args.suite])
        args.json.unlink(missing_ok=True)
        args.html.unlink(missing_ok=True)
        try:
            report = compare_measurements(read_json(args.baseline), read_json(args.candidate), read_json(args.suite))
        except InputError as error:
            report = {"schema": 1, "status": "invalid", "exit_code": 2, "benchmarks": [], "errors": [str(error)]}
        args.html.parent.mkdir(parents=True, exist_ok=True)
        args.html.write_text(render_html(report), encoding="utf-8")
        write_json_atomic(args.json, report)
        print("{}: {}".format(report["status"], "; ".join(report.get("errors", []))))
        for warning in report.get("warnings", []):
            print("warning: " + warning)
        for row in report["benchmarks"]:
            print("{}: {} ({:+.2f}%)".format(row["name"], row["status"], row["change_fraction"] * 100))
        return report["exit_code"]
    except (InputError, OSError) as error:
        print("bench_compare: " + str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
