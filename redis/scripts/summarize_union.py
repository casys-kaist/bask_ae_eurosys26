import subprocess
import glob
import re
import os
import pandas as pd

def parse_hdr(file):
    # Use -ifp for full path
    cmd = f'java -jar /home/bask_eurosys26/bask_ae_eurosys26/redis/scripts/HdrLogProcessing/target/processor.jar summarize -ifp {file}'
    result = subprocess.run(cmd, shell=True, capture_output=True, text=True)

    if result.returncode != 0:
        print(f"[ERROR] Failed to process {file}")
        print(result.stderr)
        return {}

    output = result.stdout.strip()
    if not output:
        print(f"[WARN] Empty output from summarizing {file}")
        return {}

    metrics = {}
    for line in output.splitlines():
        line = line.strip()
        match = re.match(r'(\w+(?:\.\w+)?(?:\(.*\))?)=(.+)', line)
        if match:
            key, value = match.groups()
            try:
                metrics[key.strip()] = float(value.strip())
            except ValueError:
                metrics[key.strip()] = value.strip()
    return metrics

# === Setup ===
UNIONS_DIR = "./unions"
OUTPUT_CSV = "all_union_summaries.csv"

if not os.path.isdir(UNIONS_DIR):
    print("[ERROR] unions directory not found.")
    exit(1)

group_dirs = sorted([d for d in os.listdir(UNIONS_DIR) if os.path.isdir(os.path.join(UNIONS_DIR, d))])

if not group_dirs:
    print("[INFO] No group subdirectories found under unions/")
    exit(0)

# === Collect all metrics into one DataFrame ===
all_records = []

for group in group_dirs:
    group_path = os.path.join(UNIONS_DIR, group)
    hdr_files = sorted(glob.glob(os.path.join(group_path, f'union-*.hdr')))
    
    for file in hdr_files:
        op_match = re.match(r'.*/union-([A-Z]+)_' + re.escape(group) + r'\.hdr', file)
        if op_match:
            op = op_match.group(1)
            metrics = parse_hdr(file)
            if metrics:
                metrics.update({
                    'group': group,
                    'operation': op,
                    'filename': os.path.basename(file)
                })
                all_records.append(metrics)

# === Create and save combined CSV ===
if not all_records:
    print("[INFO] No HDR metrics found. Nothing to write.")
    exit(0)

df = pd.DataFrame(all_records)

ordered_cols = ['group', 'filename', 'operation', 'Mean', '50.000ptile', '99.000ptile',
                '99.900ptile', '99.990ptile', 'Max', 'TotalCount', 'Period(ms)',
                'Throughput(ops/sec)']
available_cols = [col for col in ordered_cols if col in df.columns]

df[available_cols].sort_values(by=['group', 'operation']).to_csv(OUTPUT_CSV, index=False)
print(f"[💾] All summaries saved to: {OUTPUT_CSV}")
