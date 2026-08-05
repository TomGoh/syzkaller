#!/usr/bin/env python3
"""Render issues/INDEX.md from the ISSUE.md front-matter, validating as it goes.

Validation is the point, not a side effect: the two honesty rules in README.md
("not-observed needs a qualifying run", "root-caused needs evidence beyond
source") are only worth stating if something enforces them.

Usage: python3 issues/tools/render-index.py [--check]
       --check validates and diffs without writing.
"""

import argparse
import re
import sys
from pathlib import Path

import yaml

ISSUES = Path(__file__).resolve().parent.parent
RUNS = ISSUES / "runs"

CLASS = {"kernel-defect", "instrumentation", "operational"}
HAZARD = {"none", "wedges-target", "destroys-data"}
DIAGNOSIS = {"symptom-only", "hypothesis", "root-caused"}
DISPOSITION = {"open", "fix-proposed", "fix-verified", "not-a-defect"}
STATE = {"reproduced", "observed", "not-observed", "unknown"}

REQUIRED = ["id", "slug", "title", "class", "signature", "hazard",
            "diagnosis", "disposition", "repro", "observations"]

FRONT_MATTER = re.compile(r"\A---\n(.*?)\n---\n", re.S)


# A build string like "6.6.103+ #3 @39ee..." is the identity of an observation.
# Unquoted, YAML silently truncates it at the '#' and the record still parses --
# so the trap is invisible unless something looks for it.
H = r"[^\S\n]*"  # horizontal whitespace only -- \s* would span newlines and
                 # report the wrong key name.
UNQUOTED_HASH = re.compile(rf"""^{H}(?:-{H})?(\w+):{H}(?!['"])[^'"#\n]*#""", re.M)


def load_front_matter(path, errors=None):
    m = FRONT_MATTER.match(path.read_text(encoding="utf-8"))
    if not m:
        raise ValueError(f"{path}: no --- front-matter block at the top of the file")
    raw = m.group(1)
    if errors is not None:
        for key in UNQUOTED_HASH.findall(raw):
            errors.append(f"{path.name}: '{key}:' holds an unquoted '#' — YAML truncates "
                          f"the value there; quote it or the build identity is lost")
    return yaml.safe_load(raw)


def load_runs(errors):
    """run-id -> front-matter, for cross-checking issue claims against runs."""
    runs = {}
    for path in sorted(RUNS.glob("*.md")):
        try:
            runs[path.stem] = load_front_matter(path, errors)
        except (ValueError, yaml.YAMLError) as exc:
            errors.append(str(exc))
    return runs


def validate(issue_dir, fm, runs, errors):
    def err(msg):
        errors.append(f"{issue_dir.name}: {msg}")

    for key in REQUIRED:
        if key not in fm:
            err(f"missing required key '{key}'")
    if errors and any(issue_dir.name in e for e in errors):
        # Keep going; downstream checks tolerate absent keys via .get().
        pass

    dir_id, _, dir_slug = issue_dir.name.partition("-")
    if str(fm.get("id", "")).zfill(3) != dir_id:
        err(f"front-matter id {fm.get('id')!r} does not match directory prefix {dir_id!r}")
    if fm.get("slug") != dir_slug:
        err(f"front-matter slug {fm.get('slug')!r} does not match directory {dir_slug!r}")

    for key, allowed in (("class", CLASS), ("hazard", HAZARD),
                         ("diagnosis", DIAGNOSIS), ("disposition", DISPOSITION)):
        if key in fm and fm[key] not in allowed:
            err(f"{key}={fm[key]!r} is not one of {sorted(allowed)}")

    repro = fm.get("repro")
    if repro and repro != "none" and not (issue_dir / repro).exists():
        err(f"repro {repro!r} does not exist")

    observations = fm.get("observations") or []
    if not observations:
        err("no observations — an issue exists because something was observed")

    has_evidence = False
    for i, obs in enumerate(observations):
        where = f"observations[{i}]"
        for key in ("target", "state", "run"):
            if key not in obs:
                err(f"{where}: missing '{key}'")
        state = obs.get("state")
        if state and state not in STATE:
            err(f"{where}: state={state!r} is not one of {sorted(STATE)}")

        run_id = obs.get("run")
        if run_id and run_id not in runs:
            err(f"{where}: run {run_id!r} has no record in runs/")
        elif run_id:
            run = runs[run_id]
            issue_id = int(fm.get("id", -1))
            # Rule 1: the run must itself assert what it was capable of seeing.
            if state in ("observed", "reproduced") and issue_id not in (run.get("observed") or []):
                err(f"{where}: state={state} but run {run_id} does not list {issue_id} under 'observed'")
            if state == "not-observed" and issue_id not in (run.get("not_observed") or []):
                err(f"{where}: state=not-observed but run {run_id} does not list {issue_id} under "
                    f"'not_observed' — a run that could not observe it does not license the claim")

        ev = obs.get("evidence")
        if ev:
            if not (issue_dir / ev).exists():
                err(f"{where}: evidence {ev!r} does not exist")
            else:
                has_evidence = True

    # Rule 2: source reading alone never gets you past 'hypothesis'.
    if fm.get("diagnosis") == "root-caused" and not has_evidence:
        err("diagnosis=root-caused but no observation carries an evidence file")


def render(issues):
    out = [
        "# Issue index",
        "",
        "<!-- GENERATED by tools/render-index.py — do not hand-edit. -->",
        "",
        f"{len(issues)} issue(s). Scope and state vocabulary: [README.md](README.md).",
        "",
        "| ID | Issue | Class | State by target | Diagnosis | Disposition | Repro | Hazard |",
        "| --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for issue_dir, fm in issues:
        states = "<br>".join(
            f"`{o.get('target', '?')}` → **{o.get('state', '?')}**"
            for o in (fm.get("observations") or [])
        )
        repro = "yes" if fm.get("repro") not in (None, "none") else "—"
        hazard = fm.get("hazard", "?")
        out.append(
            f"| {str(fm.get('id')).zfill(3)} "
            f"| [{fm.get('title')}]({issue_dir.name}/ISSUE.md) "
            f"| {fm.get('class')} | {states} | {fm.get('diagnosis')} "
            f"| {fm.get('disposition')} | {repro} | {hazard} |"
        )
    out.append("")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="validate and report drift without writing INDEX.md")
    args = ap.parse_args()

    errors = []
    runs = load_runs(errors)
    issues = []
    for issue_dir in sorted(ISSUES.glob("[0-9][0-9][0-9]-*")):
        path = issue_dir / "ISSUE.md"
        if not path.is_file():
            errors.append(f"{issue_dir.name}: no ISSUE.md")
            continue
        try:
            fm = load_front_matter(path, errors)
        except (ValueError, yaml.YAMLError) as exc:
            errors.append(str(exc))
            continue
        validate(issue_dir, fm, runs, errors)
        issues.append((issue_dir, fm))

    if errors:
        print("validation failed:", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1

    index, target = render(issues), ISSUES / "INDEX.md"
    if args.check:
        current = target.read_text(encoding="utf-8") if target.exists() else ""
        if current != index:
            print(f"{target} is stale — re-run without --check", file=sys.stderr)
            return 1
        print(f"{len(issues)} issue(s) valid, INDEX.md up to date")
        return 0

    target.write_text(index, encoding="utf-8")
    print(f"{len(issues)} issue(s) valid, wrote {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
