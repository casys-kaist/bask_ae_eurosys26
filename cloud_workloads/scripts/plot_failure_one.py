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
    mark_time = None
    mark_at_points = []
    labels = []

    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--limit" and i + 1 < len(args):
            limit = int(args[i + 1])
            i += 2
        elif args[i] == "--mark" and i + 1 < len(args):
            mark = float(args[i + 1])
            i += 2
        elif args[i] == "--mark-time" and i + 1 < len(args):
            mark_time = float(args[i+1])
            i += 2
        elif args[i] == "--mark-at" and i + 2 < len(args):
            run_identifier = args[i+1]
            time_in_seconds = float(args[i+2])
            mark_at_points.append((run_identifier, time_in_seconds))
            i += 3
        elif args[i] == "--labels":
            i += 1
            while i < len(args) and not args[i].startswith("--"):
                labels.append(args[i])
                i += 1
        else:
            run_dirs.append(args[i])
            i += 1

    if not run_dirs:
        print("Usage: python plot_summary.py <dir1> <dir2> ... [--limit <sec>] [--mark <GiB>] [--mark-time <sec>] [--mark-at <run_id> <sec>] [--labels name1 name2 ...]")
        sys.exit(1)

    if labels and len(labels) != len(run_dirs):
        print("[!] Number of labels must match number of directories")
        sys.exit(1)

    return run_dirs, limit, mark, mark_time, mark_at_points, labels

def load_ksm_data(run_dir, limit):
    path = os.path.join(run_dir, KSM_FILE)
    if not os.path.exists(path):
        return None
    seconds, shared_plus_sharing = [], []
    with open(path, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            sec = int(row['second'])
            if limit is not None and sec > limit: break
            gib = ( (int(row['pages_sharing'])) * PAGE_SIZE_BYTES) / (1024 ** 3)
            seconds.append(sec)
            shared_plus_sharing.append(gib)
    return seconds, shared_plus_sharing

def load_memory_data(run_dir, limit): # Kept for completeness, though not plotted
    path = os.path.join(run_dir, MEM_FILE)
    if not os.path.exists(path):
        return None
    seconds, ram_gib = [], []
    with open(path, 'r') as f:
        reader = csv.reader(f)
        next(reader)
        for row in reader:
            sec = int(row[0])
            if limit is not None and sec > limit: break
            seconds.append(sec)
            ram_gib.append(int(row[1]) / (1024 ** 3))
    return seconds, ram_gib

def find_and_plot_specific_time_marker(ax, data_seconds, data_values, target_time_user_spec, run_color):
    if not data_seconds or not data_values or not ax:
        return

    try:
        closest_actual_sec = min(data_seconds, key=lambda s_val: abs(s_val - target_time_user_spec))
        idx_at_closest_sec = data_seconds.index(closest_actual_sec)
        val_at_closest_sec = data_values[idx_at_closest_sec]
    except (ValueError, IndexError):
        print(f"[!] Warning: Could not find data point near {target_time_user_spec}s for a run on plot '{ax.get_title()}'.")
        return

    ax.plot(closest_actual_sec, val_at_closest_sec, 'o', color=run_color, markersize=15, zorder=11, mec='black', mew=0.75)

    if abs(target_time_user_spec - round(target_time_user_spec)) < 1e-9:
        annot_text_str = f"{int(round(target_time_user_spec))}s"
    else:
        annot_text_str = f"{target_time_user_spec:.1f}s"

    ax.annotate(annot_text_str,
                (closest_actual_sec, val_at_closest_sec),
                textcoords="offset points",
                xytext=(-12, 5), # Default, adjust as needed or make smarter
                ha='right',
                va='bottom',
                fontsize=24, # Matched to other annotations
                color=run_color
               )

def plot_all_summary(run_dirs, limit, mark_gib_threshold, mark_time_global, mark_at_points_spec, labels):
    plt.rcParams.update({
        'font.size': 24, 'axes.titlesize': 28, 'axes.labelsize': 26,
        'xtick.labelsize': 24, 'ytick.labelsize': 24, 'legend.fontsize': 24
    })

    THRESHOLD_COLOR = '#E6194B' # Crimson/Red
    MARK_TIME_LINE_COLOR = '#808080' # Grey
    MARK_TIME_TEXT_COLOR = '#404040' # Dark Grey

    data_colors = ['#3498DB', '#FF6B6B', '#F39C12', '#1ABC9C'] # Blue, Light Red, Orange, Teal
    plt.rcParams['axes.prop_cycle'] = cycler(color=data_colors)

    fig, ax = plt.subplots(1, 1, figsize=(15, 6))
    
    label_map = {d: labels[i] if labels else d for i, d in enumerate(run_dirs)}

    LINE_WIDTH, VLINE_WIDTH, HLINE_WIDTH = 6, 4, 4
    gib_mark_times_colors = {} # Stores {time_when_threshold_crossed: color_of_run}

    resolved_mark_at_requests = []
    for identifier, time_val in mark_at_points_spec:
        found_run_dir = None
        if labels:
            try:
                label_idx = labels.index(identifier)
                found_run_dir = run_dirs[label_idx]
            except ValueError:
                if identifier in run_dirs:
                    found_run_dir = identifier
                    print(f"[i] Note: --mark-at identifier '{identifier}' matched a directory name, not a label (labels were provided).")
                else:
                    print(f"[!] Warning: --mark-at identifier '{identifier}' not found in provided labels: {labels}")
        else:
            if identifier in run_dirs:
                found_run_dir = identifier
            else:
                print(f"[!] Warning: --mark-at identifier '{identifier}' not found in run directories: {run_dirs}")

        if found_run_dir:
            resolved_mark_at_requests.append({'run_dir': found_run_dir, 'time': time_val})

    for idx, run_dir in enumerate(run_dirs):
        current_label = label_map[run_dir]
        current_run_color = data_colors[idx % len(data_colors)]

        ksm_data = load_ksm_data(run_dir, limit)
        
        if ksm_data:
            ksm_seconds, ksm_gib_values = ksm_data
            ax.plot(ksm_seconds, ksm_gib_values, linewidth=LINE_WIDTH, color=current_run_color, label=current_label)

            if mark_gib_threshold is not None:
                for i, val in enumerate(ksm_gib_values):
                    if val >= mark_gib_threshold:
                        mark_sec_val = ksm_seconds[i]
                        if mark_sec_val not in gib_mark_times_colors: # Store first one per run for vline
                             gib_mark_times_colors[mark_sec_val] = current_run_color
                        
                        ax.plot(mark_sec_val, val, 'o', color=current_run_color, markersize=18, zorder=10) # Circle marker

                        # Annotation for threshold crossing time
                        xytext, ha = ((-15, 5), 'right') # Default: left of point
                        if idx == 1:
                             xytext, ha = ((20, -50), 'left') # Right of point for odd indexed runs

                        ax.annotate(f"{mark_sec_val}s", (mark_sec_val, val),
                                        textcoords="offset points", xytext=xytext,
                                        fontsize=24, color=current_run_color, ha=ha, va='bottom')
                        break # Mark only the first time it crosses
            
            for request in resolved_mark_at_requests:
                if request['run_dir'] == run_dir:
                    find_and_plot_specific_time_marker(ax, ksm_seconds, ksm_gib_values,
                                                       request['time'], current_run_color)
        
    # --- Formatting KSM Plot ---
    ax.set_xlabel("Time (seconds)")
    ax.set_ylabel("Memory Savings (GiB)")
    ax.set_title("KSM Benefits Over Time")
    ax.yaxis.set_major_formatter(ScalarFormatter(useMathText=False, useOffset=False))
    ax.tick_params(axis='y', length=15, width=2)
    ax.tick_params(axis='x', length=10, width=2)
    ax.grid(False)
    
    if ax.lines: 
        ax.legend(loc='lower right')

    # --- Vertical and Horizontal Mark Lines & Y-Tick Customization ---
    if mark_gib_threshold is not None:
        ax.axhline(mark_gib_threshold, color=THRESHOLD_COLOR, linestyle='--', linewidth=HLINE_WIDTH)
        for m_sec, m_color in gib_mark_times_colors.items():
            ax.axvline(m_sec, color=m_color, linestyle='--', linewidth=VLINE_WIDTH)
        
        # Get current auto-generated ticks or ticks from a previous state
        current_yticks_before_fixed = list(ax.get_yticks())
        ax.annotate(
            f"{int(mark_gib_threshold)} GiB",
            xy=(0, mark_gib_threshold),
            xycoords=("axes fraction", "data"),
            xytext=(0, 0), textcoords='offset points',  # move right+up for clarity
            ha='left', va='bottom',
            fontsize=24, color=THRESHOLD_COLOR,
        )

    # Enforce the final desired y-limits AFTER potentially setting fixed ticks.
    # This ensures `top=85` is respected.
    # If set_yticks was called, the FixedLocator will show its ticks within [0, 85].
    # If set_yticks was not called, Matplotlib auto-generates ticks for [0, 85].
    #ax.set_ylim(bottom=0, top=85) 

    # Now, customize the labels for the y-ticks that WILL ACTUALLY BE DISPLAYED
    # after all limit and tick-setting operations.
    final_displayed_yticks = ax.get_yticks() # These are the ticks that will be shown on the y-axis
    
    yt_labels_custom = []
    yt_colors_custom = []

    for tick_val in final_displayed_yticks:
        # Format the tick label string
        lbl_str = f"{int(round(tick_val))}" if abs(tick_val - round(tick_val)) < 1e-9 else f"{tick_val:.1f}"
        yt_labels_custom.append(lbl_str)
        
        # Determine color for this tick label
        color_this_tick = 'black' # Default color
        if mark_gib_threshold is not None:
            # If a GiB threshold is marked, color the corresponding tick label
            if abs(tick_val - mark_gib_threshold) < 1e-6 * max(1, abs(mark_gib_threshold)):
                color_this_tick = THRESHOLD_COLOR
        yt_colors_custom.append(color_this_tick)

    
    # Apply custom colors and ensure font size for y-tick labels
    for tl_obj, tc_obj in zip(ax.get_yticklabels(), yt_colors_custom):
        tl_obj.set_color(tc_obj)
        tl_obj.set_fontsize(plt.rcParams['ytick.labelsize']) # Use rcParam or explicit 38

    # --mark-time (global time) lines and x-tick coloring
    if mark_time_global is not None:
        ax.axvline(mark_time_global, color=MARK_TIME_LINE_COLOR, linestyle=':', linewidth=VLINE_WIDTH, zorder=1.9)
        
        xticks = list(ax.get_xticks())
        eff_mark_x = mark_time_global
        is_present_x = any(abs(xt_val_curr - mark_time_global) < 1e-5 for xt_val_curr in xticks)
        if not is_present_x:
            xticks.append(mark_time_global)

        ax.set_xticks(sorted(list(set(xt_curr for xt_curr in xticks if xt_curr >= 0))))
        
        xt_labels, xt_colors = [], []
        for tick_val_curr in ax.get_xticks():
            lbl_str_curr = f"{int(round(tick_val_curr))}" if abs(tick_val_curr - round(tick_val_curr)) < 1e-9 else f"{tick_val_curr:.1f}"
            xt_labels.append(lbl_str_curr)
            xt_colors.append(MARK_TIME_TEXT_COLOR if abs(tick_val_curr - eff_mark_x) < 1e-5 else 'black')
        
        ax.set_xticklabels(xt_labels)
        for tl_obj_curr, tc_obj_curr in zip(ax.get_xticklabels(), xt_colors):
            tl_obj_curr.set_color(tc_obj_curr)
            # Fontsize for x-ticks is already handled by rcParams['xtick.labelsize']

        y_min_ax, y_max_ax = ax.get_ylim()
        annot_y_curr = y_min_ax + 0.02 * (y_max_ax - y_min_ax)
        if y_min_ax == 0 and y_max_ax > 0 and annot_y_curr <=0 : annot_y_curr = 0.01 * y_max_ax
        
        annot_str_curr = f"{int(round(mark_time_global))}s" if abs(mark_time_global - round(mark_time_global)) < 1e-9 else f"{mark_time_global:.1f}s"
        ax.text(mark_time_global, annot_y_curr, annot_str_curr, color=MARK_TIME_TEXT_COLOR, ha='center', va='bottom', fontsize=20)

    plt.tight_layout()
    plt.savefig("fault.svg", format="svg")
    plt.savefig("fault.pdf", format="pdf")
    print("Saved: fault.svg and fault.pdf")

if __name__ == "__main__":
    dirs, limit, mark_gib, mark_time_glob, mark_at_pts, labels_list = parse_args()
    valid_dirs = [d for d in dirs if os.path.isdir(d)]
    if not valid_dirs:
        print("[!] No valid directories provided.")
        sys.exit(1)
    if not labels_list and len(dirs) > 1 and any(True for id_val,t_val in mark_at_pts):
        print("[i] Info: Using --mark-at with multiple directories and no --labels. Identifiers for --mark-at must be directory paths.")

    plot_all_summary(valid_dirs, limit, mark_gib, mark_time_glob, mark_at_pts, labels_list)

