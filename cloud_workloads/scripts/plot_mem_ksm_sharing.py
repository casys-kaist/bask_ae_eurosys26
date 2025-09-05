#first (modified)
import os
import sys
import csv
import matplotlib.pyplot as plt
from matplotlib.ticker import ScalarFormatter
from cycler import cycler

KSM_FILE = "ksm_stats.csv"
MEM_FILE = "memory_usage.csv"
PAGE_SIZE_BYTES = 4096  # Linux default

def parse_args():
    run_dirs = []
    limit = None
    mark = None
    labels = []

    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--limit" and i + 1 < len(args):
            limit = int(args[i + 1])
            i += 2
        elif args[i] == "--mark" and i + 1 < len(args):
            mark = float(args[i + 1])  # GiB
            i += 2
        elif args[i] == "--labels":
            i += 1
            while i < len(args) and not args[i].startswith("--"):
                labels.append(args[i])
                i += 1
        else:
            run_dirs.append(args[i])
            i += 1

    if not run_dirs:
        print("Usage: python plot_summary.py <dir1> <dir2> ... [--limit <sec>] [--mark <GiB>] [--labels name1 name2 ...]")
        sys.exit(1)

    if labels and len(labels) != len(run_dirs):
        print("[!] Number of labels must match number of directories")
        sys.exit(1)

    return run_dirs, limit, mark, labels

def load_ksm_data(run_dir, limit):
    path = os.path.join(run_dir, KSM_FILE)
    if not os.path.exists(path):
        print(f"[!] Missing: {path}")
        return None

    seconds = []
    shared_plus_sharing = []

    with open(path, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            sec = int(row['second'])
            if limit is not None and sec > limit:
                break
            shared = int(row['pages_shared'])
            sharing = int(row['pages_sharing'])
            #total_pages = shared + sharing
            total_pages = sharing
            gib = (total_pages * PAGE_SIZE_BYTES) / (1024 ** 3)
            seconds.append(sec)
            shared_plus_sharing.append(gib)

    return seconds, shared_plus_sharing

def load_memory_data(run_dir, limit):
    path = os.path.join(run_dir, MEM_FILE)
    if not os.path.exists(path):
        print(f"[!] Missing: {path}")
        return None

    seconds = []
    ram_gib = []

    with open(path, 'r') as f:
        reader = csv.reader(f)
        next(reader) # Skip header
        for row in reader:
            sec = int(row[0])
            if limit is not None and sec > limit:
                break
            seconds.append(sec)
            ram_gib.append(int(row[1]) / (1024 ** 3))  # GiB

    return seconds, ram_gib

def plot_all_summary(run_dirs, limit, mark, labels):
    plt.rcParams.update({
        'font.size': 40,
        'axes.titlesize': 48,
        'axes.labelsize': 46,
        'xtick.labelsize': 38,
        'ytick.labelsize': 38,
        'legend.fontsize': 40
    })

    # Vivid red for threshold only (Kept from original script for its specific purpose)
    THRESHOLD_COLOR = '#E6194B' # A distinct color for the threshold line/tick

    # Define the new color palette for datasets
    data_colors = [
        '#2ECC71',  # Bright Green
        '#3498DB',  # Bright Blue
        '#9B59B6',  # Purple
        '#FF6B6B',  # Bright Coral Red
        '#34495E',  # Deep Slate Blue
        # Add more colors from this palette or others if you expect more datasets
    ]
    plt.rcParams['axes.prop_cycle'] = cycler(color=data_colors)

    fig, axs = plt.subplots(1, 2, figsize=(26, 12), sharex=False)
    fig.subplots_adjust(top=0.96,   # push axes up a bit
                    bottom=0.04) # give room at the bottom

    label_map = {d: labels[i] if labels else d for i, d in enumerate(run_dirs)}

    handles = []
    LINE_WIDTH = 6
    VLINE_WIDTH = 4
    HLINE_WIDTH = 4

    # Store mark times and associated colors to draw vertical lines later consistently
    mark_times_colors = {}

    for idx, run_dir in enumerate(run_dirs):
        label = label_map[run_dir]
        # Get the color assigned by the cycler for this dataset
        # (Note: this relies on plot order matching loop order)
        color = data_colors[idx % len(data_colors)]
        mark_sec = None

        ksm = load_ksm_data(run_dir, limit)
        mem = load_memory_data(run_dir, limit)

        # KSM Plot (Savings)
        if ksm:
            seconds, gib_values = ksm
            # Plot line using the color cycle
            line, = axs[0].plot(seconds, gib_values, linewidth=LINE_WIDTH, label=label)
            handles.append(line) # Add handle for potential legend on axs[0] if needed

            # Mark the threshold crossing point
            if mark is not None:
                for i, val in enumerate(gib_values):
                    if val >= mark:
                        mark_sec = seconds[i]
                        # Store the mark time and its corresponding dataset color
                        mark_times_colors[mark_sec] = color

                        # Use the specific color for this dataset's marker
                        axs[0].plot(mark_sec, val, 'o', color=color, markersize=18, zorder=10)

                        # Adjust annotation position based on index (simple example)
                        #xytext = (-15, 5) if idx % 2 != 0 else (10, 5)
                        #ha = 'right' if idx % 2 != 0 else 'left'
                        xytext = (-5, 10)
                        ha = 'right'
                        '''if idx == 0:
                            xytext = (5, 5)
                            ha = 'left'
                        '''
                        if idx == 1:
                            xytext = (8, -40)
                            ha = 'left'
                        if idx == 2:
                            xytext = (8, 10)
                            ha = 'left'
                        if idx == 3:
                            xytext = (20, -45)
                            ha = 'right'
                        axs[0].annotate(f"{mark_sec}s", (mark_sec, val),
                                        textcoords="offset points", xytext=xytext,
                                        fontsize=33, color=color, ha=ha)
                        break # Stop after finding the first crossing

        # Memory Usage Plot
        if mem:
            seconds, ram_gib = mem
            # Plot line using the color cycle
            axs[1].plot(seconds, ram_gib, linewidth=LINE_WIDTH, label=label)

    # --- Draw Lines and Format Axes ---

    # KSM Plot Formatting (axs[0])
    axs[0].set_xlabel("Time (seconds)")
    axs[0].set_ylabel("Memory Savings (GiB)")
    axs[0].set_title("KSM Benefits Over Time")
    #axs[0].set_ylim(bottom=0) # Ensure y-axis starts at 0 for savings
    axs[0].yaxis.set_major_formatter(ScalarFormatter(useMathText=False, useOffset=False))
    axs[0].tick_params(axis='y', length=15, width=2)
    axs[0].tick_params(axis='x', length=10, width=2)
    axs[0].grid(False)
    # Optional: Add legend to the first plot if needed (using handles collected)
    # axs[0].legend(handles=handles)

    # Memory Usage Plot Formatting (axs[1])
    axs[1].set_xlabel("Time (seconds)")
    axs[1].set_ylabel("Memory Usage (GiB)")
    axs[1].set_title("Memory Usage Over Time")
    #axs[1].set_ylim(bottom=0) # Ensure y-axis starts at 0 for usage
    axs[1].yaxis.set_major_formatter(ScalarFormatter(useMathText=False, useOffset=False))
    axs[1].tick_params(axis='y', length=15, width=2)
    axs[1].tick_params(axis='x', length=10, width=2)
    axs[1].legend(loc="lower right") # Legend uses labels assigned during plot calls
    axs[1].grid(False)

    # Add threshold line and vertical lines after all data is plotted
    if mark is not None:
        # Draw horizontal threshold line using the dedicated threshold color
        axs[0].axhline(mark, color=THRESHOLD_COLOR, linestyle='--', linewidth=HLINE_WIDTH)
        axs[0].annotate(
            f"{int(mark)} GiB",
            xy=(0, mark),
            xycoords=("axes fraction", "data"),
            xytext=(0, 0), textcoords='offset points',  # move right+up for clarity
            ha='left', va='bottom',
            fontsize=33, color=THRESHOLD_COLOR,
        )

        # Draw vertical lines at mark times using respective dataset colors
        for m_sec, m_color in mark_times_colors.items():
            axs[0].axvline(m_sec, color=m_color, linestyle='--', linewidth=VLINE_WIDTH)
            axs[1].axvline(m_sec, color=m_color, linestyle='--', linewidth=VLINE_WIDTH)

        yticks = list(axs[0].get_yticks())
        yticks = sorted([yt for yt in yticks if yt >= 0]) # Ensure non-negative and sorted
        axs[0].set_yticks(yticks)

        # Color the specific threshold tick label
        ytick_labels = []
        ytick_colors = []
        for tick_val in axs[0].get_yticks(): # Use the actual ticks set
            # Format tick label (avoid unnecessary decimals for integers)
            if abs(tick_val - round(tick_val)) < 1e-9:
                 label_val = f"{int(round(tick_val))}"
            else:
                 label_val = f"{tick_val:.1f}"
            ytick_labels.append(label_val)

            # Determine color
            label_color = THRESHOLD_COLOR if abs(tick_val - mark) < 1e-6 else 'black'
            ytick_colors.append(label_color)

        axs[0].set_yticklabels(ytick_labels) # Apply labels first
        # Apply colors and font size to the labels
        for tick_label, color_ in zip(axs[0].get_yticklabels(), ytick_colors):
            tick_label.set_color(color_)
            tick_label.set_fontsize(38) # Consistent font size


    plt.tight_layout()
    plt.savefig("summary.svg", format="svg")
    plt.savefig("summary.pdf", format="pdf", pad_inches=0)
    print("Saved: summary.svg and summary.pdf")

if __name__ == "__main__":
    dirs, limit, mark, labels = parse_args()
    valid_dirs = [d for d in dirs if os.path.isdir(d)]
    if not valid_dirs:
        print("[!] No valid directories provided.")
        sys.exit(1)

    plot_all_summary(valid_dirs, limit, mark, labels)

