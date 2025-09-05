import os
import time
import threading
import psutil
import csv
import subprocess
import re
import sys
import select # For non-blocking reads and pipe monitoring
import signal # For sending signals to perf process

# --- Configuration ---
# !!! IMPORTANT: Ensure this path points to your specific perf executable !!!
PERF_PATH = os.path.expanduser("/home/bask_eurosys26/bask_ae_eurosys26/codes/linux/tools/perf/perf")
PIPE_PATH = "/tmp/ram_monitor_pipe"
# Interval for perf reporting in milliseconds (-I option)
PERF_INTERVAL_MS = 1000

# !!! IMPORTANT: Prerequisites !!!
# 1. PINNING: Assumes ksmd & workloads are pinned externally to the SAME cores.
#    Example: taskset -cp 0-3 <ksmd_pid>; taskset -cp 0-3 <workload_pid>
# 2. PERMISSIONS: Run this script with 'sudo python3 your_script_name.py'
# 3. PERF PINNING (Recommended): Run this script pinned to DIFFERENT cores.
#    Example: taskset -c 4,5 sudo python3 your_script_name.py
# --- End Configuration ---

# Determine dynamic raw_data directory name (raw_data1, raw_data2, ...)
#def get_unique_data_dir(base="raw_data"):
#    suffix = 1
#    while True:
#        candidate = f"{base}{suffix}"
#        if not os.path.exists(candidate):
#            return candidate
#        suffix += 1

def get_unique_data_dir(base="raw_data"):
    tag = os.getenv("MY_CLOUD_TRIAL")
    if tag:
        base = f"raw_{tag}_"

    suffix = 1
    while True:
        candidate = f"{base}{suffix}"
        if not os.path.exists(candidate):
            return candidate
        suffix += 1

RAW_DATA_DIR = get_unique_data_dir()
start_time = time.time()

# --- Globals ---
perf_process = None
stop_event = threading.Event()
current_ksmd_pid = None # Track the PID perf is attached to

def ensure_dir():
    os.makedirs(RAW_DATA_DIR, exist_ok=True)

def read_ksm_pages():
    """Reads KSM statistics from sysfs. Returns dict."""
    stats = {'pages_scanned': -1,'pages_shared': -1,'pages_sharing': -1,'pages_volatile': -1,'pages_unshared': -1}
    paths = {'pages_scanned': "/sys/kernel/mm/ksm/pages_scanned", 'pages_shared': "/sys/kernel/mm/ksm/pages_shared", 'pages_sharing': "/sys/kernel/mm/ksm/pages_sharing", 'pages_volatile': "/sys/kernel/mm/ksm/pages_volatile", 'pages_unshared': "/sys/kernel/mm/ksm/pages_unshared"}
    for key, path in paths.items():
        try:
            with open(path, 'r') as f: stats[key] = int(f.read().strip())
        except FileNotFoundError: stats[key] = -1
        except ValueError: stats[key] = -2
        except Exception: stats[key] = -3
    return stats

def get_ksmd_pid():
    """Finds and returns the PID of the ksmd process."""
    try:
        for proc in psutil.process_iter(attrs=['pid', 'name']):
            if proc.info['name'] in ('ksmd', '[ksmd]'): return proc.info['pid']
    except psutil.Error as e:
        print(f"Warning: Error finding ksmd process: {e}", file=sys.stderr)
    return None

def start_perf_monitor(pid):
    """Starts 'perf stat -I' as a background process."""
    global perf_process, current_ksmd_pid
    if perf_process and perf_process.poll() is None:
        print("Warning: Perf process already running.")
        return True # Already running

    command = [
        PERF_PATH, 'stat',
        '-p', str(pid),
        '-e', 'cycles',       # Event to monitor
        '-I', str(PERF_INTERVAL_MS), # Reporting interval
        '--log-fd', '2'       # Force output to stderr
    ]
    print(f"Starting continuous perf: {' '.join(command)}")
    try:
        perf_process = subprocess.Popen(
            command,
            stderr=subprocess.PIPE,
            stdout=subprocess.DEVNULL, # Ignore stdout
            text=True,               # Decode stderr as text
            bufsize=1                # Line buffered stderr
        )
        # Brief pause to let perf start or fail quickly
        time.sleep(0.2)
        if perf_process.poll() is not None:
            print(f"Error: Perf process terminated immediately (code {perf_process.returncode}). Check permissions/PID.", file=sys.stderr)
            stderr_output = ""
            try: # Try reading stderr for clues
                 stderr_output = perf_process.stderr.read()
                 print(f"Perf stderr:\n{stderr_output}", file=sys.stderr)
            except Exception: pass # Ignore read errors if already closed
            perf_process = None
            return False
        current_ksmd_pid = pid # Store PID we attached to
        print(f"Perf started successfully for PID {pid}.")
        return True
    except FileNotFoundError:
        print(f"CRITICAL Error: perf executable not found at '{PERF_PATH}'.", file=sys.stderr)
        stop_event.set()
        return False
    except Exception as e:
        print(f"Error starting perf process: {e}", file=sys.stderr)
        stop_event.set()
        return False

def stop_perf_monitor():
    """Stops the background perf process gracefully."""
    global perf_process, current_ksmd_pid
    if perf_process and perf_process.poll() is None:
        print("Stopping perf process...")
        try:
            # Send SIGINT (Ctrl+C) first for graceful shutdown
            perf_process.send_signal(signal.SIGINT)
            perf_process.wait(timeout=1.5) # Wait briefly
            print("Perf exited gracefully.")
        except subprocess.TimeoutExpired:
            print("Perf did not exit via SIGINT, sending SIGTERM.")
            perf_process.terminate() # More forceful
            try:
                perf_process.wait(timeout=1.0)
            except subprocess.TimeoutExpired:
                print("Perf did not exit via SIGTERM, sending SIGKILL.")
                perf_process.kill() # Last resort
        except Exception as e:
            print(f"Error stopping perf: {e}. Trying kill.", file=sys.stderr)
            try:
                 if perf_process.poll() is None: perf_process.kill()
            except Exception: pass # Ignore kill errors
        perf_process = None
        current_ksmd_pid = None

# ==============================================================
# Updated Function to Handle <not counted>
# ==============================================================
def parse_perf_interval_line(line):
    """
    Parses a line from 'perf stat -I' output for the cycle count delta.
    Handles '<not counted>' by returning 0.
    Example line 1:  1.001188117 interval [ +- 0.001 ] ( 1.000 S ):      1,234,567      cycles
    Example line 2:  2.002097172      <not counted>      cycles
    Returns the integer cycle count delta (0 for <not counted>), or None if other parsing fails.
    """
    # 1. Check for '<not counted>' first - specific check for cycles
    if '<not counted>' in line and 'cycles' in line:
        return 0 # Treat as zero cycles due to inactivity/not counted

    # 2. If not '<not counted>', try parsing the number preceding 'cycles'
    # Regex focuses on the count preceding 'cycles', looking for colon or spaces before it
    match = re.search(r':?\s*([\d,]+)\s+cycles', line)
    if match:
        try:
            cycles_str = match.group(1).replace(',', '')
            return int(cycles_str)
        except (ValueError, IndexError):
            # print(f"Debug: Failed to parse cycles from matched string: {match.group(1)} in line: {line.strip()}", file=sys.stderr)
            return None # Parsing failed: string wasn't a valid number
    return None # Pattern (number followed by cycles) not found in line
# ==============================================================

def write_csv(path, row):
    """Appends a row to a CSV file."""
    try:
        row_str = [str(item) for item in row]
        with open(path, 'a', newline='') as f:
            writer = csv.writer(f)
            writer.writerow(row_str)
    except IOError as e:
        print(f"Error writing to CSV file {path}: {e}", file=sys.stderr)

def read_named_pipe(stop_event_local):
    """Pipe reading thread function."""
    # Create pipe if it doesn't exist
    if not os.path.exists(PIPE_PATH):
        try: os.mkfifo(PIPE_PATH)
        except OSError as e:
            print(f"Error creating pipe {PIPE_PATH}: {e}. Stopping.", file=sys.stderr)
            stop_event_local.set(); return

    pipe_fd = None
    try:
        # Open pipe non-blocking
        pipe_fd = os.open(PIPE_PATH, os.O_RDONLY | os.O_NONBLOCK)
        with os.fdopen(pipe_fd, 'r') as fifo:
            while not stop_event_local.is_set():
                # Use select to wait for data with a timeout
                readable, _, _ = select.select([fifo], [], [], 0.5) # 500ms timeout
                if readable:
                    if fifo.read(1): # Got data
                        print("\nSignal received on pipe. Stopping monitoring.")
                        stop_event_local.set()
                        break
                    else: # Pipe closed (EOF)
                        # print("Pipe closed. Stopping monitoring.") # Can be noisy
                        stop_event_local.set()
                        break
    except FileNotFoundError:
         print(f"Error: Pipe {PIPE_PATH} removed externally? Stopping.", file=sys.stderr)
         stop_event_local.set()
    except Exception as e:
        # print(f"Error reading/managing pipe {PIPE_PATH}: {e}", file=sys.stderr)
        stop_event_local.set()
    finally:
        # Ensure fd is closed if os.fdopen failed or finished early
        if pipe_fd is not None:
             try: os.close(pipe_fd)
             except OSError: pass
        # print("Pipe monitoring thread finished.") # Debug

def monitor_loop():
    """Main monitoring loop - reads perf output and other stats."""
    global current_ksmd_pid

    ensure_dir()
    print(f"Raw data will be saved in: {RAW_DATA_DIR}")
    print(f"Perf reporting interval: {PERF_INTERVAL_MS} ms")
    print("Ensure ksmd and workloads are pinned externally.")
    print("Run script with sudo and optionally pinned to separate cores.")

    # Define CSV paths
    memory_path = os.path.join(RAW_DATA_DIR, "memory_usage.csv")
    ksmd_perf_path = os.path.join(RAW_DATA_DIR, "ksmd_perf_stats.csv")
    ksm_stats_path = os.path.join(RAW_DATA_DIR, "ksm_stats.csv")

    # --- Write CSV Headers ---
    if not os.path.exists(memory_path):
        write_csv(memory_path, ["second", "ram_used_bytes", "swap_used_bytes"])
    if not os.path.exists(ksmd_perf_path):
        # Header reflects measured ksmd cycle DELTA per interval
        write_csv(ksmd_perf_path, ["second", "ksmd_cycles_delta", "ksmd_pid"])
    if not os.path.exists(ksm_stats_path):
        write_csv(ksm_stats_path, ["second", "pages_scanned", "pages_shared", "pages_sharing", "pages_volatile", "pages_unshared"])

    # --- Initial PID and Perf Start ---
    pid_to_monitor = get_ksmd_pid()
    if not pid_to_monitor:
        print("Error: ksmd process not found at startup. Cannot start perf.", file=sys.stderr)
        stop_event.set() # Signal exit if PID not found initially
        return
    if not start_perf_monitor(pid_to_monitor):
        # start_perf_monitor already printed error and set stop_event
        return

    print(f"Monitoring started. Reading perf stream from PID {current_ksmd_pid}...")
    loop_count = 0

    # --- Main Loop: Read perf output ---
    while not stop_event.is_set():
        # Check if perf process died unexpectedly
        if perf_process.poll() is not None:
            print(f"Error: Perf process terminated unexpectedly (code {perf_process.returncode}). Stopping.", file=sys.stderr)
            stop_event.set()
            break

        # Use select to wait for data on stderr with a timeout
        stderr_fd = perf_process.stderr.fileno()
        readable, _, _ = select.select([stderr_fd], [], [], 0.5) # 500ms timeout

        if not readable:
            # Timeout occurred, check if ksmd PID still exists
            # (perf might hang if pid disappears without perf noticing)
            if current_ksmd_pid and not psutil.pid_exists(current_ksmd_pid):
                 print(f"Error: Monitored ksmd PID {current_ksmd_pid} disappeared. Stopping perf.", file=sys.stderr)
                 stop_event.set() # Signal stop
            continue # Go back to select and check stop_event

        # Data is available on stderr
        try:
            line = perf_process.stderr.readline()
            if not line: # Empty read typically means pipe closed / process ended
                if perf_process.poll() is None:
                     print("Warning: Perf stderr stream ended unexpectedly but process still running?", file=sys.stderr)
                else:
                     print("Perf process stderr stream ended (process terminated).")
                stop_event.set() # Signal stop
                break

            # --- Process Perf Line ---
            second = int(time.time() - start_time) # Timestamp for this interval end
            cycles_delta = parse_perf_interval_line(line)

            if cycles_delta is not None:
                # Successfully parsed cycles delta (could be 0 for <not counted>)
                write_csv(ksmd_perf_path, [
                    second,
                    cycles_delta,
                    current_ksmd_pid # Log the PID being monitored
                ])

                # --- Read Other Stats Concurrently ---
                # These are read approx. when the perf interval ends
                try:
                    current_mem = psutil.virtual_memory()
                    current_swap = psutil.swap_memory()
                    write_csv(memory_path, [second, current_mem.used, current_swap.used])

                    ksm_stats = read_ksm_pages()
                    write_csv(ksm_stats_path, [
                        second,
                        ksm_stats['pages_scanned'], ksm_stats['pages_shared'],
                        ksm_stats['pages_sharing'], ksm_stats['pages_volatile'],
                        ksm_stats['pages_unshared']
                    ])
                except psutil.Error as e:
                     print(f"Warning: psutil error reading mem/swap stats at second {second}: {e}", file=sys.stderr)
                except Exception as e:
                     print(f"Warning: Failed to read/write mem/ksm stats at second {second}: {e}", file=sys.stderr)


                loop_count += 1
                if loop_count % 20 == 0: print(".", end='', flush=True) # Print dot every 20 seconds

            # else: # Line wasn't a cycle count line (header, footer, etc.)
            #     print(f"Debug: Non-cycle line: {line.strip()}", file=sys.stderr) # Uncomment for deep debug
            #     pass

        except Exception as e:
            print(f"\nError reading or processing perf output line: {e}", file=sys.stderr)
            # Consider if this error is fatal
            # stop_event.set() # Option: stop on any read error
            break # Exit loop on error for now

    print("\nMonitoring loop finished.")

def main():
    # --- Pre-run Checks ---
    if not os.path.isfile(PERF_PATH) or not os.access(PERF_PATH, os.X_OK):
         print(f"CRITICAL Error: perf not found or not executable at '{PERF_PATH}'", file=sys.stderr)
         sys.exit(1)
    if os.geteuid() != 0:
        print("Warning: Script not running as root. 'perf' needs root privileges.", file=sys.stderr)

    # --- Setup Threads ---
    # Setup stop event first
    global stop_event
    stop_event = threading.Event()

    # Start the pipe reader thread
    pipe_thread = threading.Thread(target=read_named_pipe, args=(stop_event,), daemon=True)
    pipe_thread.start()

    # --- Start Monitoring ---
    try:
        # Run monitor_loop directly in main thread
        monitor_loop()

    except KeyboardInterrupt:
        print("\nCtrl+C detected. Stopping monitoring...")
        stop_event.set() # Signal threads/loops to stop
    except Exception as e:
         print(f"\nAn unexpected error occurred in the main execution: {e}", file=sys.stderr)
         stop_event.set() # Ensure stop on unexpected error
    finally:
        # --- Cleanup ---
        print("Initiating cleanup...")
        # Make sure stop_event is set for threads
        stop_event.set()
        # Stop the perf process FIRST
        stop_perf_monitor()
        print("Waiting for background threads...")
        # Wait for pipe thread to finish
        if pipe_thread.is_alive():
            pipe_thread.join(timeout=1.5) # Wait max 1.5 seconds for pipe thread

        print("Exiting main program.")

if __name__ == "__main__":
    main()


# --- Post-processing Calculations (Example for CPU Utilization) ---
# Place this in a separate Python script or Jupyter Notebook.

"""
import pandas as pd
import os
import numpy as np

# --- Configuration for Analysis ---
RAW_DATA_DIR = "raw_data4" # <<< CHANGE to your actual raw_data directory name
PERF_INTERVAL_SEC = 1.0    # Must match PERF_INTERVAL_MS / 1000 from main script

# !!! Crucial for Utilization Calculation !!!
# You MUST determine the total available cycles per interval on the cores
# where ksmd/workload were pinned.
# Option 1: Estimate using nominal frequency (LESS ACCURATE)
NOMINAL_CPU_FREQ_HZ = 3.0e9 # Example: 3.0 GHz <<< CHANGE THIS to your CPU's base frequency
NUM_PINNED_CORES = 4       # Example: Pinned to 4 cores <<< CHANGE THIS
TOTAL_AVAILABLE_CYCLES_PER_INTERVAL = NOMINAL_CPU_FREQ_HZ * NUM_PINNED_CORES * PERF_INTERVAL_SEC

# Option 2: Measure separately (MORE ACCURATE)
# Run 'perf stat -C <pinned_cores> -e cycles -- sleep <Interval_Sec*N>' multiple times
# under similar load to get average measured cycles per Interval_Sec on those cores.
# TOTAL_AVAILABLE_CYCLES_PER_INTERVAL = Measured value (e.g., 11.5e9 cycles/sec for 4 cores * 1 sec)

print(f"Using estimated {TOTAL_AVAILABLE_CYCLES_PER_INTERVAL=:.2e} cycles/interval for utilization.")
# ---

def analyze_utilization_continuous(data_dir):
    ksmd_perf_path = os.path.join(data_dir, "ksmd_perf_stats.csv")
    if not os.path.exists(ksmd_perf_path):
        print(f"Error: ksmd_perf_stats.csv not found in '{data_dir}'")
        return

    try:
        print(f"Loading data from {ksmd_perf_path}...")
        perf_df = pd.read_csv(ksmd_perf_path)

        # --- Data Cleaning ---
        # Cycles column contains the DELTA per interval
        # Ensure correct column name ('ksmd_cycles_delta') is used
        if 'ksmd_cycles_interval' in perf_df.columns and 'ksmd_cycles_delta' not in perf_df.columns:
             perf_df.rename(columns={'ksmd_cycles_interval': 'ksmd_cycles_delta'}, inplace=True)

        if 'ksmd_cycles_delta' not in perf_df.columns:
             print(f"Error: Column 'ksmd_cycles_delta' not found in {ksmd_perf_path}")
             return

        perf_df['ksmd_cycles_delta'] = pd.to_numeric(perf_df['ksmd_cycles_delta'], errors='coerce')
        perf_df.dropna(subset=['ksmd_cycles_delta'], inplace=True) # Drop rows where parsing failed
        # Ensure cycles are non-negative (should be from perf -I deltas, includes 0 for <not counted>)
        perf_df = perf_df[perf_df['ksmd_cycles_delta'] >= 0].copy()

        if perf_df.empty:
            print("No valid cycle delta measurements found after cleaning.")
            return

        # --- Calculate CPU Utilization per Interval ---
        # Utilization = (Cycles Delta in Interval) / (Total Available Cycles in Interval)
        perf_df['ksmd_cpu_utilization_percent'] = (
            perf_df['ksmd_cycles_delta'] / TOTAL_AVAILABLE_CYCLES_PER_INTERVAL
        ) * 100

        # --- Display Results ---
        print("\n--- Analysis Results (Continuous Perf) ---")
        print(f"Analyzed data from: {data_dir}")
        print(f"Found {len(perf_df)} valid measurement intervals.")

        print("\nSample calculated data:")
        print(perf_df[['second', 'ksmd_cycles_delta', 'ksmd_cpu_utilization_percent']].head())

        # Calculate overall average utilization
        avg_utilization = perf_df['ksmd_cpu_utilization_percent'].mean()
        median_utilization = perf_df['ksmd_cpu_utilization_percent'].median()
        max_utilization = perf_df['ksmd_cpu_utilization_percent'].max()

        # Calculate utilization only when ksmd was active (cycles > 0)
        active_intervals = perf_df[perf_df['ksmd_cycles_delta'] > 0]
        avg_utilization_when_active = active_intervals['ksmd_cpu_utilization_percent'].mean() if not active_intervals.empty else 0

        print(f"\nOverall Average ksmd CPU Utilization: {avg_utilization:.4f}%")
        print(f"Overall Median ksmd CPU Utilization:  {median_utilization:.4f}%")
        print(f"Overall Maximum ksmd CPU Utilization: {max_utilization:.4f}%")
        print(f"Average ksmd CPU Utilization (when active > 0 cycles): {avg_utilization_when_active:.4f}%")

        # Save processed data (optional)
        # output_csv_path = os.path.join(data_dir, "utilization_analysis_continuous.csv")
        # perf_df.to_csv(output_csv_path, index=False)
        # print(f"\nAnalysis results saved to: {output_csv_path}")

    except FileNotFoundError as e:
        print(f"Error loading file: {e}", file=sys.stderr)
    except Exception as e:
        print(f"An error occurred during analysis: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()

# --- Run the analysis ---
# analyze_utilization_continuous(RAW_DATA_DIR)
"""
