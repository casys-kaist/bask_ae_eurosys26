#!/usr/bin/env python3
"""
parse_failures.py

Adds a "before_stabilize" aggregate (indexes 0..3) to the printed JSON.
"""

import argparse
import csv
import json
import re
from pathlib import Path
from typing import List, Dict, Any

TS_SPLIT_RE = re.compile(r'(?=\[\s*\d+\.\d+\])')
FAIL_STATS_RE = re.compile(r'\[Failure Statistics\],\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)')
REASONS_HEADER_RE = re.compile(r'Merge failure reasons:\s*$')
REASON_LINE_RE = re.compile(r'\[\s*\d+\.\d+\]\s*\[\s*([A-Za-z0-9_ ]+)\s*\]\s*,\s*(\d+)\s*$')

def normalize_chunks(raw: str) -> List[str]:
    return [c.strip() for c in TS_SPLIT_RE.split(raw) if c.strip()]

def parse_file(text: str) -> List[Dict[str, Any]]:
    chunks = normalize_chunks(text)
    results: List[Dict[str, Any]] = []
    i = 0
    while i < len(chunks):
        chunk = chunks[i]
        m = FAIL_STATS_RE.search(chunk)
        if not m:
            i += 1
            continue

        stable_merge = int(m.group(1))
        unstable_merge = int(m.group(2))
        explicit_fail = int(m.group(3))
        implicit_fail = int(m.group(4))

        record: Dict[str, Any] = {
            "stable_merge": stable_merge,
            "unstable_merge": unstable_merge,
            "explicit_fail": explicit_fail,
            "implicit_fail": implicit_fail,
            "reasons": {}
        }

        # Collect reasons after "Merge failure reasons:" (if any)
        reasons: Dict[str, int] = {}
        j = i + 1
        saw_header = False
        while j < len(chunks):
            nxt = chunks[j]
            if not saw_header:
                if REASONS_HEADER_RE.search(nxt):
                    saw_header = True
                    j += 1
                    continue
                if FAIL_STATS_RE.search(nxt):
                    break
                j += 1
                continue

            rm = REASON_LINE_RE.search(nxt)
            if rm:
                reasons[rm.group(1).strip()] = int(rm.group(2))
                j += 1
                continue

            if FAIL_STATS_RE.search(nxt) or REASONS_HEADER_RE.search(nxt):
                break
            break

        record["reasons"] = reasons
        results.append(record)
        i += 1

    return results

def _norm_reason(name: str) -> str:
    return re.sub(r'\s+', ' ', name.replace('_', ' ')).strip().lower()

def _fmt_pct(num: float | None) -> str:
    return "NA" if num is None else f"{num*100:.2f}%"

def compute_stats(block: Dict[str, Any]) -> Dict[str, Any]:
    stable = block["stable_merge"]
    unstable = block["unstable_merge"]
    explicit = block["explicit_fail"]
    implicit = block["implicit_fail"]
    reasons = block.get("reasons", {})

    total_trials = stable + unstable + explicit + implicit
    total_fail = explicit + implicit

    pnid = sum(int(cnt) for r, cnt in reasons.items()
               if _norm_reason(r) == "pages are not identical")

    fail_rate = (total_fail / total_trials) if total_trials > 0 else None
    pnid_rate = (pnid / total_fail) if total_fail > 0 else None
    implicit_rate = (implicit / total_fail) if total_fail > 0 else None

    etc_count = max(explicit - pnid, 0)
    etc_rate = (etc_count / total_fail) if total_fail > 0 else None

    return {
        "stable_merge": stable,
        "unstable_merge": unstable,
        "explicit_fail": explicit,
        "implicit_fail": implicit,
        "total_trials": total_trials,
        "total_fail": total_fail,
        "pages_not_identical": pnid,
        "etc_count": etc_count,
        "total_fail_rate": _fmt_pct(fail_rate),
        "pages_not_identical_rate": _fmt_pct(pnid_rate),
        "implicit_rate": _fmt_pct(implicit_rate),
        "etc_rate": _fmt_pct(etc_rate),
        "_raw": {
            "total_fail_rate": fail_rate,
            "pages_not_identical_rate": pnid_rate,
            "implicit_rate": implicit_rate,
            "etc_rate": etc_rate,
        },
    }

def compute_total(blocks: List[Dict[str, Any]]) -> Dict[str, Any]:
    stable = sum(b["stable_merge"] for b in blocks)
    unstable = sum(b["unstable_merge"] for b in blocks)
    explicit = sum(b["explicit_fail"] for b in blocks)
    implicit = sum(b["implicit_fail"] for b in blocks)

    pnid = 0
    for b in blocks:
        for rname, cnt in b.get("reasons", {}).items():
            if _norm_reason(rname) == "pages are not identical":
                pnid += int(cnt)

    total_trials = stable + unstable + explicit + implicit
    total_fail = explicit + implicit
    etc_count = max(explicit - pnid, 0)

    fail_rate = (total_fail / total_trials) if total_trials > 0 else None
    pnid_rate = (pnid / total_fail) if total_fail > 0 else None
    implicit_rate = (implicit / total_fail) if total_fail > 0 else None
    etc_rate = (etc_count / total_fail) if total_fail > 0 else None

    return {
        "stable_merge": stable,
        "unstable_merge": unstable,
        "explicit_fail": explicit,
        "implicit_fail": implicit,
        "total_trials": total_trials,
        "total_fail": total_fail,
        "pages_not_identical": pnid,
        "etc_count": etc_count,
        "total_fail_rate": _fmt_pct(fail_rate),
        "pages_not_identical_rate": _fmt_pct(pnid_rate),
        "implicit_rate": _fmt_pct(implicit_rate),
        "etc_rate": _fmt_pct(etc_rate),
        "_raw": {
            "total_fail_rate": fail_rate,
            "pages_not_identical_rate": pnid_rate,
            "implicit_rate": implicit_rate,
            "etc_rate": etc_rate,
        },
    }

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logfile", type=Path, help="Path to the log file to parse")
    ap.add_argument("--csv", type=Path,
                    help="Optional path to write per-index CSV plus TOTAL row (unchanged)")
    args = ap.parse_args()

    text = args.logfile.read_text(encoding="utf-8", errors="ignore")
    blocks_raw = parse_file(text)

    per_index = [compute_stats(b) for b in blocks_raw]
    totals_all = compute_total(blocks_raw)

    # ---- NEW: "Before Stabilize" aggregate using indexes 0..3 ----
    before_slice = blocks_raw[:3]  # handles files with <4 indexes gracefully
    before_stabilize = compute_total(before_slice)
    # Add a small hint on how many indexes went in
    before_stabilize["_indexes_used"] = len(before_slice)

    out = {
        "total_indexes": len(per_index),
        "indexes": per_index,
        "before_stabilize": before_stabilize,   # <-- printed as requested
        "total": totals_all
    }
    print(json.dumps(out, indent=2))

    # CSV behavior unchanged (per-index rows + final TOTAL row)
    if args.csv:
        fieldnames = [
            "stable_merge",
            "unstable_merge",
            "explicit_fail",
            "implicit_fail",
            "total_trials",
            "total_fail",
            "pages_not_identical",
            "etc_count",
            "total_fail_rate",
            "pages_not_identical_rate",
            "implicit_rate",
            "etc_rate",
        ]
        with args.csv.open("w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=fieldnames)
            w.writeheader()
            for row in per_index:
                w.writerow({k: v for k, v in row.items() if k in fieldnames})
            w.writerow({k: v for k, v in totals_all.items() if k in fieldnames})

if __name__ == "__main__":
    main()

