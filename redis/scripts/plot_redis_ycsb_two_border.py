import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import argparse
import math
import re

# --- Configuration Definitions ---
BASELINE_CONFIG_NAME = 'no_ksm'
# Non-baseline configurations intended for plotting comparisons
CONFIG_DISPLAY_NAMES = ['1.25k-20', '4k-20', 'dataplane', 'bask', 'bask_opt']
# Full list including baseline, defining plot order when baseline is shown
ALL_DISPLAY_CONFIGS_ORDERED = [BASELINE_CONFIG_NAME] + CONFIG_DISPLAY_NAMES

# Mapping from internal config names to CSV prefixes
ALL_CONFIG_PREFIX_MAP = {
    'no_ksm': 'no_ksm_',
    '1.25k-20': '1.25k-20_',
    '4k-20': '4k-20_',
    'dataplane': 'dataplane_',
    'bask': 'bask_',
    'bask_opt': 'bask_opt_'
}

A = ['No-KSM', '1250-20ms', '4000-20ms', 'DataPlane', 'BASK', 'BASK-OPT']
ALL_CONFIG_LEGENDS_NAME = {
    'no_ksm': 'No-KSM',
    '1.25k-20': '1250-20ms',
    '4k-20': '4000-20ms',
    'dataplane': 'DataPlane',
    'bask': 'BASK',
    'bask_opt': 'BASK-OPT'
}

# --- Color Palette ---
NEW_COLORS = [
    '#2ECC71',  # Bright Green   -> 1.25k-20
    '#3498DB',  # Bright Blue    -> 4k-20
    '#9B59B6',  # Purple         -> styx
    '#FF6B6B',  # Bright Coral Red -> bask
    '#34495E',  # Deep Slate Blue   -> bask_opt
]
BASELINE_COLOR = "#FFB347" # Bright Orange for no_ksm

# Map non-baseline display names to new colors
CONFIG_COLORS = {name: color for name, color in zip(CONFIG_DISPLAY_NAMES, NEW_COLORS)}
# Combined map for easy lookup in plotting function
ALL_CONFIG_COLORS = {BASELINE_CONFIG_NAME: BASELINE_COLOR, **CONFIG_COLORS}


# --- Workload Definitions ---
WORKLOAD_OPERATIONS_ORDER = [
    ('YCSB-a', ['READ', 'UPDATE']),
    ('YCSB-b', ['READ', 'UPDATE']),
    ('YCSB-c', ['READ']),
    ('YCSB-d', ['READ', 'INSERT']),
    ('Geomean', ['Overall'])
]
WORKLOAD_SUFFIX_MAP = {
    'YCSB-a': '_a',
    'YCSB-b': '_b',
    'YCSB-c': '_c',
    'YCSB-d': '_d',
}

# --- Valid Metrics Mapping ---
# Added 'is_tail' flag to control normalization and baseline plotting
METRIC_MAPPING = {
    'average': {'col': 'Mean', 'name': 'Average Latency', 'short': 'Average', 'is_tail': True}, # Changed is_tail to False for average
    'p99': {'col': '99.000ptile', 'name': 'P99 Latency', 'short': 'p99', 'is_tail': True},
    'p99.9': {'col': '99.900ptile', 'name': 'P99.9 Latency', 'short': 'p99.9', 'is_tail': True},
    'p99.99': {'col': '99.990ptile', 'name': 'P99.99 Latency', 'short': 'p99.99', 'is_tail': True},
}

# --- Data Processing Functions ---
def calculate_geometric_mean(series):
    """Calculates the geometric mean of a pandas Series, ignoring NaNs and non-positive values."""
    positive_values = series[series > 1e-9].dropna() # Use a small threshold > 0
    if positive_values.empty:
        return np.nan
    try:
        log_values = np.log(positive_values)
        if np.any(~np.isfinite(log_values)): return np.nan # Check after log
        mean_log = np.mean(log_values)
        if not np.isfinite(mean_log): return np.nan # Check after mean
        result = np.exp(mean_log)
        return result if np.isfinite(result) else np.nan
    except (OverflowError, ValueError):
        return np.inf

def preprocess_data(csv_filepath, metric_key):
    """
    Preprocesses data from a CSV file for a specific metric.

    Args:
        csv_filepath (str): Path to the input CSV file.
        metric_key (str): The key of the metric to process (e.g., 'p99', 'average').

    Returns:
        pandas.DataFrame: A DataFrame ready for plotting, either with raw values
                          (for tail metrics) or normalized values (for average).
                          Includes baseline column only for tail metrics.
                          Returns an empty DataFrame or None on error.
    """
    metric_info = METRIC_MAPPING.get(metric_key.lower())
    if not metric_info:
        print(f"Error: Invalid metric key '{metric_key}' provided to preprocess_data.")
        return None
    metric_col_csv = metric_info['col']
    is_tail_metric = metric_info['is_tail']

    try:
        df_raw_csv = pd.read_csv(csv_filepath)
    except FileNotFoundError:
        print(f"Error: CSV file '{csv_filepath}' not found.")
        return None
    except Exception as e:
        print(f"Error reading CSV file '{csv_filepath}': {e}")
        return None

    if 'operation' not in df_raw_csv.columns or 'group' not in df_raw_csv.columns:
        print(f"Error: Missing required columns ('operation' or 'group') in {csv_filepath}.")
        return None
    df_raw_csv['operation'] = df_raw_csv['operation'].str.upper()

    # --- Data Extraction ---
    ycsb_raw_data_list_all_configs = []
    required_configs_for_metric = ALL_DISPLAY_CONFIGS_ORDERED

    for display_workload_name, ops_to_plot_for_workload in WORKLOAD_OPERATIONS_ORDER:
        if display_workload_name == 'Geomean': continue
        workload_suffix_csv = WORKLOAD_SUFFIX_MAP.get(display_workload_name)
        if not workload_suffix_csv: continue

        for op_csv_format in ops_to_plot_for_workload:
            op_display_format = op_csv_format.capitalize()
            current_row_values = {'Workload': display_workload_name, 'Operation': op_display_format}
            found_any_data_for_op = False

            for config_name_iter, config_prefix_csv in ALL_CONFIG_PREFIX_MAP.items():
                csv_group_identifier = config_prefix_csv + workload_suffix_csv.lstrip('_')
                filtered_rows = df_raw_csv[
                    (df_raw_csv['group'] == csv_group_identifier) &
                    (df_raw_csv['operation'] == op_csv_format)
                ]

                if not filtered_rows.empty:
                    if metric_col_csv not in filtered_rows.columns:
                        value = np.nan
                    else:
                        value_raw = filtered_rows[metric_col_csv].iloc[0]
                        value = pd.to_numeric(value_raw, errors='coerce')
                    current_row_values[config_name_iter] = value
                    if config_name_iter in required_configs_for_metric:
                         found_any_data_for_op = True
                else:
                    current_row_values[config_name_iter] = np.nan
            
            if found_any_data_for_op:
                 if any(pd.notna(current_row_values.get(cfg)) for cfg in required_configs_for_metric):
                     ycsb_raw_data_list_all_configs.append(current_row_values)

    if not ycsb_raw_data_list_all_configs:
        if any(wl_op[0] == 'Geomean' for wl_op in WORKLOAD_OPERATIONS_ORDER):
            placeholder_geomean = {'Workload': 'Geomean', 'Operation': 'Overall'}
            cols_for_metric = ALL_DISPLAY_CONFIGS_ORDERED if is_tail_metric else CONFIG_DISPLAY_NAMES
            for cfg_plot in cols_for_metric: placeholder_geomean[cfg_plot] = np.nan
            final_cols = ['Workload', 'Operation'] + cols_for_metric
            return pd.DataFrame([placeholder_geomean], columns=final_cols)
        else:
             return pd.DataFrame()

    ycsb_df_raw_all_configs = pd.DataFrame(ycsb_raw_data_list_all_configs)

    # --- Processing Based on Metric Type ---
    if is_tail_metric:
        result_df = ycsb_df_raw_all_configs[['Workload', 'Operation'] + ALL_DISPLAY_CONFIGS_ORDERED].copy()
        columns_for_geomean = ALL_DISPLAY_CONFIGS_ORDERED
        df_for_geomean_calc = result_df
    else: # Average metric: Normalize
        result_df = ycsb_df_raw_all_configs[['Workload', 'Operation']].copy()
        columns_for_geomean = CONFIG_DISPLAY_NAMES

        if BASELINE_CONFIG_NAME not in ycsb_df_raw_all_configs.columns:
            print(f"Error: Baseline '{BASELINE_CONFIG_NAME}' column not found for normalization. Setting normalized values to NaN.")
            for config_plot_col in CONFIG_DISPLAY_NAMES: result_df[config_plot_col] = np.nan
        elif ycsb_df_raw_all_configs[BASELINE_CONFIG_NAME].isnull().all():
            print(f"Warning: Baseline '{BASELINE_CONFIG_NAME}' column is all NaN. Normalization will result in NaN.")
            for config_plot_col in CONFIG_DISPLAY_NAMES: result_df[config_plot_col] = np.nan
        else:
            for index, row_raw_data in ycsb_df_raw_all_configs.iterrows():
                baseline_val = row_raw_data.get(BASELINE_CONFIG_NAME)
                if pd.notna(baseline_val) and abs(baseline_val) > 1e-9:
                    for config_plot_col in CONFIG_DISPLAY_NAMES:
                        if config_plot_col in row_raw_data:
                            target_val = row_raw_data[config_plot_col]
                            result_df.loc[index, config_plot_col] = target_val / baseline_val if pd.notna(target_val) else np.nan
                        else:
                            result_df.loc[index, config_plot_col] = np.nan
                else:
                    for config_plot_col in CONFIG_DISPLAY_NAMES:
                        result_df.loc[index, config_plot_col] = np.nan
        df_for_geomean_calc = result_df

    # --- Calculate Geomean ---
    if any(wl_op[0] == 'Geomean' for wl_op in WORKLOAD_OPERATIONS_ORDER):
        geomean_row_data = {'Workload': 'Geomean', 'Operation': 'Overall'}
        if not df_for_geomean_calc.empty:
            valid_rows_for_geomean = df_for_geomean_calc[df_for_geomean_calc['Workload'] != 'Geomean']
            for config_col_name in columns_for_geomean:
                if config_col_name in valid_rows_for_geomean.columns:
                    gmean = calculate_geometric_mean(valid_rows_for_geomean[config_col_name])
                    geomean_row_data[config_col_name] = gmean
                else:
                    geomean_row_data[config_col_name] = np.nan
        else:
            for config_col_name in columns_for_geomean:
                geomean_row_data[config_col_name] = np.nan
        
        geomean_df = pd.DataFrame([geomean_row_data])
        if 'Workload' not in geomean_df: geomean_df['Workload'] = 'Geomean'
        if 'Operation' not in geomean_df: geomean_df['Operation'] = 'Overall'
        result_df = pd.concat([result_df, geomean_df], ignore_index=True)

    # --- Final Sorting ---
    final_plot_order_tuples = []
    for wl_name_order, op_list_order in WORKLOAD_OPERATIONS_ORDER:
        for op_name_order_csv in op_list_order:
            final_plot_order_tuples.append((wl_name_order, op_name_order_csv.capitalize()))

    if not result_df.empty and not result_df[['Workload', 'Operation']].isnull().all().all():
        result_df['_sort_key_tuple'] = list(zip(result_df['Workload'], result_df['Operation']))
        actual_categories_present_in_data = [
             cat_tuple for cat_tuple in final_plot_order_tuples
             if cat_tuple in result_df['_sort_key_tuple'].tolist()
        ]
        if actual_categories_present_in_data:
            result_df['_sort_category'] = pd.Categorical(
                result_df['_sort_key_tuple'],
                categories=actual_categories_present_in_data,
                ordered=True
            )
            result_df = result_df.sort_values('_sort_category').drop(
                 columns=['_sort_key_tuple', '_sort_category']
            ).reset_index(drop=True)
        else:
            result_df = result_df.drop(columns=['_sort_key_tuple']).reset_index(drop=True)
    elif not result_df.empty:
         result_df = result_df.reset_index(drop=True)

    # --- Ensure Final Columns ---
    if is_tail_metric:
        expected_final_cols = ['Workload', 'Operation'] + ALL_DISPLAY_CONFIGS_ORDERED
    else:
        expected_final_cols = ['Workload', 'Operation'] + CONFIG_DISPLAY_NAMES
    for col in expected_final_cols:
        if col not in result_df.columns:
            result_df[col] = np.nan
    return result_df[expected_final_cols]


# --- Chart Drawing Function ---
def draw_chart(data_dfs, metric_keys, metric_displays, y_labels, output_pdf_path=None,
               fontsize_title=12, fontsize_ylabel=12, fontsize_xticklabel=10,
               fontsize_workloadlabel=11, fontsize_legend=10, fontsize_yticklabel=10):
    """
    Draws one or two bar charts vertically stacked with specific styling.
    Handles both normalized (average) and raw (tail) metrics.

    Args:
        data_dfs (list): List of pandas DataFrames, one for each metric.
        metric_keys (list): List of metric keys (e.g., 'p99', 'average') corresponding to data_dfs.
        metric_displays (list): List of title strings for each chart.
        y_labels (list): List of y-axis label strings for each chart.
        output_pdf_path (str, optional): Path to save the output PDF. Defaults to None.
        fontsize_title (float): Font size for plot titles.
        fontsize_ylabel (float): Font size for Y-axis labels.
        fontsize_xticklabel (float): Font size for X-axis tick labels (operation names).
        fontsize_workloadlabel (float): Font size for X-axis workload labels.
        fontsize_legend (float): Font size for the legend.
        fontsize_yticklabel (float): Font size for Y-axis tick numbers.
    """
    num_plots = len(data_dfs)
    if num_plots == 0:
        print("No data provided to draw_chart.")
        return
    if not (len(metric_keys) == num_plots and len(metric_displays) == num_plots and len(y_labels) == num_plots):
         print("Error: Mismatch between number of dataframes, keys, labels, or titles.")
         return

    fig, axes = plt.subplots(nrows=num_plots, ncols=1,
                             figsize=(10, 3.5 * num_plots),
                             sharex=False)

    if num_plots == 1: axes = [axes]
    fig_handles, fig_labels = None, None

    for idx, (ax, data_df, metric_key, metric_name_display, y_axis_label) in enumerate(zip(axes, data_dfs, metric_keys, metric_displays, y_labels)):

        metric_info = METRIC_MAPPING[metric_key]
        is_normalized = not metric_info['is_tail']

        if is_normalized:
            configs_to_plot_in_group = CONFIG_DISPLAY_NAMES
            legend_labels_ordered = CONFIG_DISPLAY_NAMES
        else:
            configs_to_plot_in_group = ALL_DISPLAY_CONFIGS_ORDERED
            legend_labels_ordered = ALL_DISPLAY_CONFIGS_ORDERED

        if data_df is None or data_df.empty:
            print(f"Warning: No data available for plot {idx+1} ('{metric_key}'). Skipping.")
            ax.text(0.5, 0.5, f"No data for\n'{metric_key}'", ha='center', va='center', transform=ax.transAxes, fontsize=12, color='gray')
            ax.set_yticks([]); ax.set_xticks([]); ax.spines[:].set_visible(False)
            continue

        for cfg_col in configs_to_plot_in_group:
             if cfg_col not in data_df.columns:
                 data_df[cfg_col] = np.nan

        temp_df = data_df.dropna(subset=configs_to_plot_in_group, how='all').reset_index(drop=True)
        if temp_df.empty:
            print(f"Warning: No non-empty data rows left for plot {idx+1} ('{metric_key}') after filtering. Skipping.")
            ax.text(0.5, 0.5, f"No valid data for\n'{metric_key}'", ha='center', va='center', transform=ax.transAxes, fontsize=12, color='gray')
            ax.set_yticks([]); ax.set_xticks([]); ax.spines[:].set_visible(False)
            continue
        data_df = temp_df
        num_plotted_configs_per_group = len(configs_to_plot_in_group)
        num_plot_groups = len(data_df)

        bar_width = 0.18
        intra_config_group_spacing = 0.0
        total_width_per_plot_group = num_plotted_configs_per_group * bar_width + max(0, num_plotted_configs_per_group - 1) * intra_config_group_spacing
        inter_plot_group_spacing = bar_width * 2.5

        operation_tick_positions = []
        operation_tick_labels = []
        workload_spans = {}
        current_x_axis_offset = 0
        last_major_workload_for_separator = None
        current_workload_start_x = 0

        for i in range(num_plot_groups):
            workload_name = data_df.loc[i, 'Workload']
            operation_name = data_df.loc[i, 'Operation']
            is_geomean_group = (workload_name == "Geomean" and operation_name == "Overall")

            if last_major_workload_for_separator is not None and workload_name != last_major_workload_for_separator:
                separator_line_pos = current_x_axis_offset - (inter_plot_group_spacing / 2.0) - 0.1
                ax.axvline(separator_line_pos, color='dimgray', linestyle='-', linewidth=1.0,
                           ymin=-0.3, ymax=0.0, clip_on=False, zorder=0)
                if last_major_workload_for_separator not in workload_spans:
                     workload_spans[last_major_workload_for_separator] = {'start': current_workload_start_x - 0.3, 'end': separator_line_pos - intra_config_group_spacing/2}
                current_workload_start_x = current_x_axis_offset
            elif i == 0:
                current_workload_start_x = current_x_axis_offset
            last_major_workload_for_separator = workload_name

            operation_group_center_x = current_x_axis_offset + (total_width_per_plot_group / 2.0) - 0.1
            operation_tick_positions.append(operation_group_center_x)
            op_label = "" if is_geomean_group else operation_name
            operation_tick_labels.append(op_label)

            for j, config_name_iter in enumerate(configs_to_plot_in_group):
                metric_value = data_df.loc[i, config_name_iter]
                bar_hatch = '///' if pd.isna(metric_value) else None
                metric_value_plot = 0 if pd.isna(metric_value) else metric_value
                bar_x_position = current_x_axis_offset + j * (bar_width + intra_config_group_spacing)
                bar_color = ALL_CONFIG_COLORS.get(config_name_iter, '#808080')
                label_for_legend = config_name_iter if i == 0 and fig_handles is None else ""
                ax.bar(bar_x_position, metric_value_plot, width=bar_width,
                       label=label_for_legend, color=bar_color, hatch=bar_hatch, zorder=2,
                       edgecolor='black', linewidth=0.5)

            current_x_axis_offset += total_width_per_plot_group + inter_plot_group_spacing

        if last_major_workload_for_separator not in workload_spans:
           last_group_end_x = current_x_axis_offset - inter_plot_group_spacing
           workload_spans[last_major_workload_for_separator] = {'start': current_workload_start_x, 'end': last_group_end_x}

        ax.set_ylabel(y_axis_label, fontsize=fontsize_ylabel, linespacing=1.0)
        ax.yaxis.set_label_coords(-0.10, 0.5)
        ax.grid(axis='y', linestyle=':', linewidth=0.7, alpha=0.6, zorder=0)
        ax.set_axisbelow(True)
        if is_normalized:
             ax.axhline(1.0, color='dimgray', linestyle='--', linewidth=1.2, zorder=1)

        ax.set_title(metric_name_display, fontsize=fontsize_title, pad=3)

        if is_normalized:
            fixed_upper_limit = 1.5
            step = 0.25
            ax.set_ylim(0, fixed_upper_limit)
            new_calculated_ticks = np.arange(0, fixed_upper_limit + step, step)
            new_calculated_ticks = new_calculated_ticks[new_calculated_ticks <= fixed_upper_limit + 1e-9]
            y_ticks_final_fixed = np.unique(np.round(np.concatenate((np.array([0.0, 1.0]), new_calculated_ticks)), decimals=5))
            ax.set_yticks(y_ticks_final_fixed)
        else:
            max_val = data_df[configs_to_plot_in_group].max().max()
            if pd.isna(max_val) or max_val <= 1e-9:
                 ax.set_ylim(0, 1)
            else:
                 ax.set_ylim(0, max_val * 1.1)
        ax.tick_params(axis='y', labelsize=fontsize_yticklabel)

        ax.set_xticks(operation_tick_positions)
        ax.set_xticklabels(operation_tick_labels, fontsize=fontsize_xticklabel, ha='center')
        ax.tick_params(axis='x', which='both', bottom=False, top=False, length=0)
        ax.spines['bottom'].set_visible(True)
        ax.spines['bottom'].set_color('dimgray')
        ax.spines['left'].set_visible(True)
        ax.spines['right'].set_visible(False)
        ax.spines['top'].set_visible(False)

        y_workload_label = -0.18
        for workload, span in workload_spans.items():
            center_x = (span['start'] + span['end']) / 2.0
            label_text = "Geomean" if workload == "Geomean" else workload
            ax.text(center_x, y_workload_label, label_text,
                    ha='center', va='top', fontsize=fontsize_workloadlabel, weight='medium',
                    transform=ax.get_xaxis_transform())

        if fig_handles is None:
             handles, labels = ax.get_legend_handles_labels()
             if handles:
                 handle_dict = dict(zip(labels, handles))
                 ordered_handles = []
                 ordered_labels = []
                 for lbl in legend_labels_ordered:
                     if lbl in handle_dict:
                         ordered_handles.append(handle_dict[lbl])
                         ordered_labels.append(lbl)
                 if ordered_handles:
                    fig_handles, fig_labels = ordered_handles, ordered_labels

    if fig_handles:
        num_legend_items = len(fig_labels)
        fig.legend(fig_handles, A, loc='upper center', ncol=num_legend_items,
                   bbox_to_anchor=(0.5, 1.01),
                   frameon=False, fontsize=fontsize_legend, columnspacing=0.5)

    #plt.tight_layout(rect=[0.02, 0.08, 0.98, 0.93], h_pad=3.5)
    plt.tight_layout(rect=[0, 0, 1, 0.85], h_pad=3)

    if output_pdf_path:
        try:
            plt.savefig(output_pdf_path, format='pdf', bbox_inches='tight')
            print(f"\nChart saved to {output_pdf_path}")
        except Exception as e:
            print(f"Error saving chart to PDF '{output_pdf_path}': {e}")
    else:
        plt.show()


# --- Main Execution ---
def main():
    parser = argparse.ArgumentParser(description="Generate bar chart(s) from performance CSV data. Normalizes 'average' latency, shows raw 'tail' latencies (p99*).")
    parser.add_argument('--metric', type=str, required=True, action='append',
                        help=f"Metric(s) to plot (specify multiple times, max 2). Choose from: {', '.join(METRIC_MAPPING.keys())}.")
    parser.add_argument('--csv', type=str, default='all_union_summaries.csv',
                        help="Path to the input CSV file (default: all_union_summaries.csv).")
    parser.add_argument('--output_pdf', type=str, default=None,
                        help="Optional: Path to save the output chart(s) as a PDF file.")
    
    # Fontsize arguments
    parser.add_argument('--fontsize_title', type=float, default=12, help="Font size for plot titles (default: 12).")
    parser.add_argument('--fontsize_ylabel', type=float, default=12, help="Font size for Y-axis labels (default: 12).")
    parser.add_argument('--fontsize_xticklabel', type=float, default=10, help="Font size for X-axis tick labels (operation names) (default: 10).")
    parser.add_argument('--fontsize_workloadlabel', type=float, default=11, help="Font size for X-axis workload labels (e.g., YCSB-a) (default: 11).")
    parser.add_argument('--fontsize_legend', type=float, default=10, help="Font size for the legend (default: 10).")
    parser.add_argument('--fontsize_yticklabel', type=float, default=10, help="Font size for Y-axis tick numbers (default: 10).")


    args = parser.parse_args()

    # --- Validate and Prepare Metrics ---
    selected_metric_keys_raw = args.metric
    if not (1 <= len(selected_metric_keys_raw) <= 2):
        parser.error("Please provide exactly one or two metrics using the --metric argument (e.g., --metric p99 --metric average).")

    metric_keys_to_process = [] 
    metric_display_names_on_chart = []
    y_axis_labels_on_chart = []      

    print(f"--- Chart Generation Configuration ---")
    print(f"Input CSV File: '{args.csv}'")
    print(f"Normalization Baseline: '{BASELINE_CONFIG_NAME}' (Applied only to 'average' metric)")
    print(f"Plotting Configurations:")
    print(f"  - Baseline (Raw Tail Plots): {BASELINE_CONFIG_NAME} ({BASELINE_COLOR})")
    for name, color in CONFIG_COLORS.items():
        print(f"  - Comparison: {name} ({color})")
    print(f"Selected Metrics:")

    metrics_info_str = []
    for metric_key_raw in selected_metric_keys_raw:
        metric_key = metric_key_raw.lower()
        metric_info = METRIC_MAPPING.get(metric_key)
        if not metric_info:
            parser.error(f"Invalid metric '{metric_key_raw}'. Choose from: {', '.join(METRIC_MAPPING.keys())}")

        metric_keys_to_process.append(metric_key)
        is_tail = metric_info['is_tail']
        base_metric_name = metric_info['name']
        short_metric_name = metric_info['short']

        if is_tail:
            y_axis_labels_on_chart.append(f'Latency (µs)')
            metric_display_names_on_chart.append(f'{base_metric_name}')
            metrics_info_str.append(f"  - {metric_key.upper()}: Raw Values (µs)")
        else:
            y_axis_labels_on_chart.append(f'{short_metric_name} Latency\nNorm. to {BASELINE_CONFIG_NAME}')
            metric_display_names_on_chart.append(f'Normalized {base_metric_name} (vs {BASELINE_CONFIG_NAME})')
            metrics_info_str.append(f"  - {metric_key.upper()}: Normalized to '{BASELINE_CONFIG_NAME}'")

    for info_str in metrics_info_str: print(info_str)
    
    print(f"Font Sizes:")
    print(f"  - Title: {args.fontsize_title}pt")
    print(f"  - Y-axis Label: {args.fontsize_ylabel}pt")
    print(f"  - Y-axis Tick Numbers: {args.fontsize_yticklabel}pt")
    print(f"  - X-axis Tick Labels (Operations): {args.fontsize_xticklabel}pt")
    print(f"  - X-axis Workload Labels: {args.fontsize_workloadlabel}pt")
    print(f"  - Legend: {args.fontsize_legend}pt")

    if args.output_pdf:
        print(f"Output PDF: '{args.output_pdf}'")
    print("--------------------------------------")

    # --- Process Data for Each Metric ---
    data_frames_for_chart = []
    valid_data_found_overall = False
    print("\n--- Data Preprocessing ---")
    for i, metric_key in enumerate(metric_keys_to_process):
        print(f"[{i+1}/{len(metric_keys_to_process)}] Processing data for metric: '{metric_key}'...")
        df = preprocess_data(args.csv, metric_key)
        data_frames_for_chart.append(df)

        if df is not None and not df.empty:
            metric_info = METRIC_MAPPING[metric_key]
            is_tail = metric_info['is_tail']
            if is_tail:
                expected_cols = ALL_DISPLAY_CONFIGS_ORDERED
            else:
                expected_cols = CONFIG_DISPLAY_NAMES
            cols_present = [col for col in expected_cols if col in df.columns]
            if cols_present and df[cols_present].notna().any().any():
                 print(f"  -> Valid data found for '{metric_key}'.")
                 valid_data_found_overall = True
            else:
                print(f"  -> Warning: No valid numeric data found in relevant columns for '{metric_key}' after processing.")
        else:
             print(f"  -> Warning: Preprocessing returned no DataFrame for '{metric_key}'.")
    print("--------------------------")

    if not valid_data_found_overall:
         print("\nError: No valid data found for any selected metric after preprocessing. Aborting chart generation.")
         print("-------------------------------------------------------------------------------------")
         return

    print("\n--- Generating Chart(s) ---")
    draw_chart(data_frames_for_chart,
               metric_keys_to_process,
               metric_display_names_on_chart,
               y_axis_labels_on_chart,
               output_pdf_path=args.output_pdf,
               fontsize_title=args.fontsize_title,
               fontsize_ylabel=args.fontsize_ylabel,
               fontsize_xticklabel=args.fontsize_xticklabel,
               fontsize_workloadlabel=args.fontsize_workloadlabel,
               fontsize_legend=args.fontsize_legend,
               fontsize_yticklabel=args.fontsize_yticklabel)
    print("---------------------------")


if __name__ == '__main__':
    main()

