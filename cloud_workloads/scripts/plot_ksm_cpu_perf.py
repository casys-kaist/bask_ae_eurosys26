import os
import sys
import csv
import matplotlib.pyplot as plt
from matplotlib.ticker import ScalarFormatter
import numpy as np # For calculating averages and standard deviation

# --- Configuration for Plotting ---
# This MUST match the fixed frequency of the pinned cores after disabling dynamic scaling
# and the number of cores ksmd is effectively utilizing (usually the number it's pinned to).
FIXED_FREQ_HZ = 2.9e9  # 2900 MHz = 2.9 GHz
NUM_PINNED_CORES = 1     # <<< IMPORTANT: Adjust this to the number of cores ksmd is pinned to and expected to saturate if 100% busy.
                           # If ksmd is pinned to multiple cores (e.g., 0-3), NUM_PINNED_CORES = 4.
                           # If ksmd is a single-threaded process effectively using one core among many it's allowed on, this is more complex.
                           # For a single-threaded kernel thread like ksmd, even if pinned to multiple cores,
                           # its utilization is best expressed against ONE core's capacity if it doesn't parallelize.
                           # If you are measuring ksmd pinned to a *set* of cores and want its utilization across that *set*,
                           # then NUM_PINNED_CORES should be the size of that set.
PERF_INTERVAL_SEC = 1.0  # Interval used in the perf monitoring script (e.g., PERF_INTERVAL_MS / 1000)

# Calculate total available cycles in one interval on the pinned core(s)
# This is the denominator for utilization percentage.
TOTAL_AVAILABLE_CYCLES_PER_INTERVAL = FIXED_FREQ_HZ * NUM_PINNED_CORES * PERF_INTERVAL_SEC
# --- End Configuration ---


def parse_args():
    import argparse
    parser = argparse.ArgumentParser(description="Plot ksmd CPU utilization (%) from multiple raw_data directories using perf cycle data.")
    parser.add_argument("dirs", nargs="+", help="List of raw_data directories (e.g., raw_data1 raw_data2)")
    parser.add_argument("--limit", type=int, default=None, help="Optional time limit in seconds to truncate data for plotting")
    parser.add_argument("--output", type=str, default="ksmd_cpu_utilization_perf.png", help="Output PNG filename")
    return parser.parse_args()

def plot_ksmd_cpu_utilization(dirs, time_limit=None, output_filename="ksmd_cpu_utilization_perf.png"):
    plt.figure(figsize=(12, 7)) # Slightly wider for potentially multiple lines

    all_stats = {} # To store avg and std for each directory

    for d_idx, data_dir in enumerate(dirs):
        perf_stats_path = os.path.join(data_dir, "ksmd_perf_stats.csv")
        if not os.path.exists(perf_stats_path):
            print(f"[!] Skipping: {perf_stats_path} not found")
            continue

        print(f"Processing directory: {data_dir}")
        seconds = []
        ksmd_cycles_deltas = []

        try:
            with open(perf_stats_path, 'r') as f:
                reader = csv.DictReader(f)
                if "second" not in reader.fieldnames or "ksmd_cycles_delta" not in reader.fieldnames:
                    print(f"[!] Error: Missing required columns ('second', 'ksmd_cycles_delta') in {perf_stats_path}. Skipping.")
                    continue

                for row_num, row in enumerate(reader):
                    try:
                        sec = int(row["second"])
                        # ksmd_cycles_delta should already be 0 for <not counted> or the actual delta
                        cycles_delta = float(row["ksmd_cycles_delta"])

                        if time_limit is not None and sec > time_limit:
                            break
                        seconds.append(sec)
                        ksmd_cycles_deltas.append(cycles_delta)
                    except ValueError as e:
                        print(f"[!] Warning: Skipping row {row_num+1} in {perf_stats_path} due to value error: {e} - Row: {row}")
                        continue
        except Exception as e:
            print(f"[!] Error reading or processing {perf_stats_path}: {e}")
            continue

        if not seconds:
            print(f"[!] No valid data found in {perf_stats_path} for {data_dir}")
            continue

        # Convert lists to numpy arrays for easier calculation
        seconds_np = np.array(seconds)
        ksmd_cycles_deltas_np = np.array(ksmd_cycles_deltas)

        # Calculate CPU utilization percentage for each interval
        cpu_utilization_percent = (ksmd_cycles_deltas_np / TOTAL_AVAILABLE_CYCLES_PER_INTERVAL) * 100
        cpu_utilization_percent = np.clip(cpu_utilization_percent, 0, 100.0) # Clip at 100% of defined capacity


        # Calculate average and standard deviation for this directory
        if len(cpu_utilization_percent) > 0:
            avg_util = np.mean(cpu_utilization_percent)
            std_util = np.std(cpu_utilization_percent) # Calculate standard deviation

            # Stats for active periods
            active_util_percent = cpu_utilization_percent[ksmd_cycles_deltas_np > 0]
            if len(active_util_percent) > 0:
                avg_util_active = np.mean(active_util_percent)
                std_util_active = np.std(active_util_percent)
            else:
                avg_util_active = 0.0
                std_util_active = 0.0
        else:
            avg_util = 0.0
            std_util = 0.0
            avg_util_active = 0.0
            std_util_active = 0.0

        all_stats[data_dir] = {
            'overall_avg': avg_util,
            'overall_std': std_util,
            'active_avg': avg_util_active,
            'active_std': std_util_active
        }

        label = (f"{os.path.basename(data_dir)} "
                 f"(Avg: {avg_util:.2f}±{std_util:.2f}%, "
                 f"Active Avg: {avg_util_active:.2f}±{std_util_active:.2f}%)")
        plt.plot(seconds_np, cpu_utilization_percent, label=label, linewidth=1.2, alpha=0.8)

    if not all_stats: # No data was successfully plotted
        print("[!] No data plotted from any directory.")
        plt.close() # Close the empty figure
        return

    plt.xlabel("Time (seconds)")
    plt.ylabel(f"ksmd CPU Utilization (% of {NUM_PINNED_CORES} Core(s) Capacity @ {FIXED_FREQ_HZ/1e9:.1f}GHz)")
    plt.title("ksmd CPU Utilization Over Time (from perf cycles)")
    plt.grid(True, linestyle='--', alpha=0.7)
    plt.legend(loc='best', fontsize='small') # Adjust legend location and font if needed

    plt.ylim(bottom=0, top=max(20, plt.gca().get_ylim()[1] * 1.05))

    ax = plt.gca()
    formatter = ScalarFormatter(useMathText=False)
    formatter.set_scientific(False)
    formatter.set_powerlimits((-3, 3))
    ax.yaxis.set_major_formatter(formatter)

    plt.tight_layout()
    plt.savefig(output_filename)
    print(f"\nPlot saved to: {output_filename}")

    print("\n--- CPU Utilization Statistics ---")
    for d, stats in all_stats.items():
        print(f"Directory: {os.path.basename(d)}")
        print(f"  Overall Average: {stats['overall_avg']:.2f}%")
        print(f"  Overall Std Dev: {stats['overall_std']:.2f}%")
        print(f"  Active Average (when cycles > 0): {stats['active_avg']:.2f}%")
        print(f"  Active Std Dev (when cycles > 0): {stats['active_std']:.2f}%")

if __name__ == "__main__":
    args = parse_args()

    print(f"--- Configuration ---")
    print(f"Fixed CPU Frequency: {FIXED_FREQ_HZ/1e9:.2f} GHz")
    print(f"Number of Pinned Cores for Denominator: {NUM_PINNED_CORES}")
    print(f"Perf Interval: {PERF_INTERVAL_SEC} sec")
    print(f"Total Available Cycles per Interval (per {NUM_PINNED_CORES} core(s)): {TOTAL_AVAILABLE_CYCLES_PER_INTERVAL:.2e}")
    print(f"-----------------------\n")

    plot_ksmd_cpu_utilization(args.dirs, args.limit, args.output)

