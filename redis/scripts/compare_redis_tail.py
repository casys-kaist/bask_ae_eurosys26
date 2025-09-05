#!/usr/bin/env python3
# filepath: /home/ljhhasang/BASK_AE/plot_tbl/compare_lat.py

import pandas as pd
import numpy as np
import sys
import argparse

def calculate_geometric_mean(series):
    """Calculates the geometric mean of a pandas Series, ignoring NaNs and non-positive values."""
    positive_values = series[series > 1e-9].dropna()
    if positive_values.empty:
        return np.nan
    try:
        log_values = np.log(positive_values)
        if np.any(~np.isfinite(log_values)): 
            return np.nan
        mean_log = np.mean(log_values)
        if not np.isfinite(mean_log): 
            return np.nan
        result = np.exp(mean_log)
        return result if np.isfinite(result) else np.nan
    except (OverflowError, ValueError):
        return np.nan

def get_config_geomean(csv_file, config_name, metric_col='99.900ptile'):
    """Get geometric mean for a configuration"""
    try:
        df = pd.read_csv(csv_file)
    except:
        return None
    
    # Filter for the specific configuration
    config_data = df[df['group'].str.startswith(config_name + '_')].copy()
    
    if config_data.empty:
        return None
    
    # Extract latencies for workloads a,b,c,d with READ/UPDATE/INSERT operations
    workload_ops = [
        ('a', ['READ', 'UPDATE']),
        ('b', ['READ', 'UPDATE']),
        ('c', ['READ']),
        ('d', ['READ', 'INSERT'])
    ]
    
    latencies = []
    for workload_suffix, operations in workload_ops:
        for operation in operations:
            matching_rows = config_data[
                (config_data['group'].str.endswith(f'_{workload_suffix}')) &
                (config_data['operation'] == operation)
            ]
            
            if not matching_rows.empty and metric_col in matching_rows.columns:
                latency = pd.to_numeric(matching_rows[metric_col].iloc[0], errors='coerce')
                if pd.notna(latency) and latency > 0:
                    latencies.append(latency)
    
    if not latencies:
        return None
    
    return calculate_geometric_mean(pd.Series(latencies))

def main():
    parser = argparse.ArgumentParser(description="Compare P99.9 latency geometric means.")
    parser.add_argument('baseline', help="Baseline configuration (usually best performing)")
    parser.add_argument('target', help="Target configuration to compare")
    parser.add_argument('--csv', default='all_union_summaries.csv', help="CSV file path")
    parser.add_argument('--metric', default='99.900ptile', help="Latency metric column")
    
    args = parser.parse_args()
    
    baseline_geomean = get_config_geomean(args.csv, args.baseline, args.metric)
    target_geomean = get_config_geomean(args.csv, args.target, args.metric)
    
    if baseline_geomean is None or target_geomean is None:
        print("Error: Could not calculate geometric means")
        sys.exit(1)
    
    ratio = target_geomean / baseline_geomean
    
    print(f"{args.baseline}: {baseline_geomean:.2f}µs")
    print(f"{args.target}: {target_geomean:.2f}µs")
    print(f"{args.target} is {ratio:.2f}x vs {args.baseline}")

if __name__ == "__main__":
    main()

