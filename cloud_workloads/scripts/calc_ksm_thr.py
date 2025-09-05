import os
import sys
import csv

KSM_STATS_FILE = "ksm_stats.csv"
KSMD_PERF_FILE = "ksmd_perf_stats.csv"

def print_usage_and_exit():
    print("Usage: python script.py <dir1> ... --limit <sec> [--labels <l1> ...]")
    sys.exit(1)

def parse_args():
    run_dirs = []
    limit_sec = None
    labels = []
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--limit" and i + 1 < len(args):
            try:
                limit_sec = int(args[i + 1])
            except ValueError:
                print(f"[!] Error: --limit '{args[i+1]}' not int.")
                sys.exit(1)
            i += 2
        elif args[i] == "--labels":
            i += 1
            while i < len(args) and not args[i].startswith("--"):
                labels.append(args[i]); i += 1
        elif args[i].startswith("--"):
            print(f"[!] Unknown option: {args[i]}")
            print_usage_and_exit()
        else:
            run_dirs.append(args[i]); i += 1

    if not run_dirs: print("[!] No directories."); print_usage_and_exit()
    if limit_sec is None: print("[!] --limit <sec> required."); print_usage_and_exit()
    if labels and len(labels) != len(run_dirs):
        print("[!] Label count mismatch with initial directories."); sys.exit(1)
    return run_dirs, limit_sec, labels

def safe_read_int(row, key):
    try:
        return int(row[key])
    except Exception:
        return None

def get_final_pages_scanned(run_dir, limit_sec):
    path = os.path.join(run_dir, KSM_STATS_FILE)
    if not os.path.exists(path): return None, "Missing ksm_stats.csv"
    latest_pages, found_data = -1, False
    prev_s, prev_p = -1, -1
    first_row = True
    try:
        with open(path, 'r', newline='') as f:
            reader = csv.DictReader(f)
            for row in reader:
                s = safe_read_int(row, 'second')
                p = safe_read_int(row, 'pages_scanned')
                if s is None or p is None:
                    continue
                if not first_row and s >= prev_s and p < prev_p:
                    # Non-cumulative; proceed silently
                    pass
                if p >= 0:
                    prev_p, prev_s = p, s
                    first_row = False
                if s <= limit_sec:
                    latest_pages, found_data = p, True
                else:
                    break
        if not found_data:
            return None, f"No pages <= {limit_sec}s"
        return latest_pages, None
    except Exception as e:
        return None, f"ReadErr:{str(e)[:20]}"

def get_total_ksmd_cycles(run_dir, limit_sec):
    path = os.path.join(run_dir, KSMD_PERF_FILE)
    if not os.path.exists(path): return None, "Missing ksmd_perf_stats.csv"
    total_cyc, found_data = 0, False
    try:
        with open(path, 'r', newline='') as f:
            reader = csv.DictReader(f)
            for row in reader:
                s = safe_read_int(row, 'second')
                c_delta = safe_read_int(row, 'ksmd_cycles_delta')
                if s is None or c_delta is None:
                    continue
                if s <= limit_sec:
                    total_cyc += c_delta
                    found_data = True
        if not found_data and limit_sec > 0:
            return 0, f"No cycles <= {limit_sec}s"
        return total_cyc, None
    except Exception as e:
        return None, f"ReadErr:{str(e)[:20]}"

def calculate(run_dirs, limit_sec, labels):
    results = []
    baseline_total_throughput = None
    baseline_effective_throughput = None
    baseline_usec_per_page = None

    first_tt_set = False
    first_et_set = False
    first_uspp_set = False

    for i, run_dir in enumerate(run_dirs):
        label = labels[i] if labels else os.path.basename(run_dir.rstrip('/'))
        row_error = None

        if not os.path.isdir(run_dir):
            results.append({
                "label": label, "final_pages_scanned": None, "total_ksmd_cycles": None,
                "total_throughput": None, "effective_throughput": None,
                "cycles_per_page": None, "seconds_per_page": None,  # stores µs/page
                "error": "Dir not found"
            })
            continue

        pages, err_p = get_final_pages_scanned(run_dir, limit_sec)
        cycles, err_c = get_total_ksmd_cycles(run_dir, limit_sec)

        row_error = err_p or err_c

        total_tput = None
        eff_tput = None
        cpp = None
        uspp = None  # microseconds per page

        if pages is not None:
            if limit_sec > 0:
                total_tput = pages / limit_sec
                if pages > 0:
                    # µs/page = (seconds / page) * 1e6
                    uspp = (limit_sec * 1_000_000) / pages
            # else: total_tput/uspp remain None

        if cycles is not None and pages is not None:
            if pages > 0:
                cpp = cycles / pages
            if cycles > 0:
                eff_tput = pages / cycles

        # establish baselines from the first eligible rows
        if not first_tt_set and total_tput is not None:
            baseline_total_throughput = total_tput
            first_tt_set = True
        if not first_et_set and eff_tput is not None:
            baseline_effective_throughput = eff_tput
            first_et_set = True
        if not first_uspp_set and uspp is not None:
            baseline_usec_per_page = uspp
            first_uspp_set = True

        results.append({
            "label": label,
            "final_pages_scanned": pages,
            "total_ksmd_cycles": cycles,
            "total_throughput": total_tput,
            "effective_throughput": eff_tput,
            "cycles_per_page": cpp,
            "seconds_per_page": uspp,  # NOTE: now holds µs/page
            "error": row_error
        })

    return results, baseline_total_throughput, baseline_effective_throughput, baseline_usec_per_page

def only_print_table(results, baseline_tt, baseline_et, baseline_uspp, limit_sec):
    header_format = "| {:<25} | {:>18} | {:>20} | {:>26} | {:>32} | {:>20} | {:>24} |"
    row_format    = "| {:<25} | {:>18} | {:>20} | {:>26} | {:>32} | {:>20} | {:>24} |"
    header = header_format.format(
        "Label", "Pages Scanned", "KSMD Cycles",
        "Total Tput(pg/s) (Rel.)", "Effective Tput(pg/cyc) (Rel.)",
        "Cycles/Page(cyc/pg)", "Microseconds/Page(µs/pg) (Rel.)"
    )
    print(header)
    print("-" * len(header))

    # determine baseline labels for (1.00x) display
    tt_baseline_label = None
    et_baseline_label = None
    uspp_baseline_label = None

    if baseline_tt is not None:
        for r in results:
            if r["total_throughput"] == baseline_tt and r["error"] is None:
                tt_baseline_label = r["label"]; break
    if baseline_et is not None:
        for r in results:
            if r["effective_throughput"] == baseline_et and r["error"] is None:
                et_baseline_label = r["label"]; break
    if baseline_uspp is not None:
        for r in results:
            if r["seconds_per_page"] == baseline_uspp and r["error"] is None:
                uspp_baseline_label = r["label"]; break

    tt_base_shown = False
    et_base_shown = False
    uspp_base_shown = False

    for r in results:
        if r["error"]:
            err = f"Err:{r['error'][:10]}.."
            ps_str = tc_str = tt_str = et_str = cpp_str = spp_str = err
        else:
            ps_str = f"{r['final_pages_scanned']:,}" if r['final_pages_scanned'] is not None else "N/A"
            tc_str = f"{r['total_ksmd_cycles']:,}" if r['total_ksmd_cycles'] is not None else "N/A"

            # Total throughput
            if r["total_throughput"] is not None:
                tv = f"{r['total_throughput']:,.2f}"
                if baseline_tt and baseline_tt > 0:
                    if r["label"] == tt_baseline_label and not tt_base_shown:
                        tt_str = f"{tv} (1.00x)"; tt_base_shown = True
                    else:
                        tt_str = f"{tv} ({r['total_throughput']/baseline_tt:.2f}x)"
                else:
                    tt_str = tv
            else:
                tt_str = "N/A" if limit_sec > 0 else "N/A (lim<=0)"

            # Effective throughput (pg/cyc)
            if r["effective_throughput"] is not None:
                ev = f"{r['effective_throughput']:.8f}"
                if baseline_et and baseline_et > 0:
                    if r["label"] == et_baseline_label and not et_base_shown:
                        et_str = f"{ev} (1.00x)"; et_base_shown = True
                    else:
                        et_str = f"{ev} ({r['effective_throughput']/baseline_et:.2f}x)"
                else:
                    et_str = ev
            else:
                if r['final_pages_scanned'] is not None and r['total_ksmd_cycles'] == 0:
                    et_str = "N/A (Cyc=0)"
                else:
                    et_str = "N/A"

            # Cycles per page
            if r["cycles_per_page"] is not None:
                cpp_str = f"{r['cycles_per_page']:,.2f}"
            else:
                if r['total_ksmd_cycles'] is not None and r['final_pages_scanned'] == 0:
                    cpp_str = "N/A (Pg=0)"
                else:
                    cpp_str = "N/A"

            # Microseconds per page (Rel.)  <-- r["seconds_per_page"] now stores µs/page
            if r["seconds_per_page"] is not None:
                uv = f"{r['seconds_per_page']:.2f}"
                if baseline_uspp and baseline_uspp > 0:
                    if r["label"] == uspp_baseline_label and not uspp_base_shown:
                        spp_str = f"{uv} (1.00x)"; uspp_base_shown = True
                    else:
                        spp_str = f"{uv} ({r['seconds_per_page']/baseline_uspp:.2f}x)"
                else:
                    spp_str = uv
            else:
                if r['final_pages_scanned'] is not None:
                    if r['final_pages_scanned'] == 0 and limit_sec > 0:
                        spp_str = "N/A (Pg=0)"
                    elif limit_sec <= 0:
                        spp_str = "N/A (lim<=0)"
                    else:
                        spp_str = "N/A"
                else:
                    spp_str = "N/A"

        print(row_format.format(r["label"], ps_str, tc_str, tt_str, et_str, cpp_str, spp_str))
    print("-" * len(header))

def main():
    run_dirs_orig, limit_sec, labels_orig = parse_args()

    valid_run_dirs = []
    valid_labels = [] if labels_orig else None
    for i, d in enumerate(run_dirs_orig):
        if os.path.isdir(d):
            valid_run_dirs.append(d)
            if labels_orig:
                valid_labels.append(labels_orig[i])

    if not valid_run_dirs:
        print("[!] No valid directories found to process.")
        sys.exit(1)

    if labels_orig and len(valid_labels) != len(valid_run_dirs):
        print("[!] Error: Label count does not match valid directory count after filtering.")
        sys.exit(1)

    results, base_tt, base_et, base_uspp = calculate(
        valid_run_dirs, limit_sec, valid_labels if labels_orig else []
    )
    only_print_table(results, base_tt, base_et, base_uspp, limit_sec)

if __name__ == "__main__":
    main()


