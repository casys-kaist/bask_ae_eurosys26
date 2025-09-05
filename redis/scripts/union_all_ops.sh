#!/bin/bash

# === Configuration ===
BASE_DIR="./redis-benchmark-results"
OUTPUT_BASE="./unions"
PROCESSOR_JAR="/home/bask_eurosys26/bask_ae_eurosys26/redis/scripts/HdrLogProcessing/target/processor.jar"

# === Argument check ===
if [ $# -lt 1 ]; then
  echo "Usage: $0 <prefix1> [<prefix2> ...]"
  exit 1
fi

# === Loop over each provided prefix ===
for PREFIX in "$@"; do
  SUFFIX="_$PREFIX"
  WORKDIRS=()

  # Exact directory
  if [ -d "$BASE_DIR/$PREFIX/logs" ]; then
    WORKDIRS+=("$BASE_DIR/$PREFIX/logs")
  else
    # Prefix match
    for dir in "$BASE_DIR"/${PREFIX}_*; do
      if [ -d "$dir/logs" ]; then
        WORKDIRS+=("$dir/logs")
      fi
    done
  fi

  if [ ${#WORKDIRS[@]} -eq 0 ]; then
    echo "[ERROR] No matching logs directories found for '$PREFIX'"
    continue
  fi

  echo "[INFO] Found ${#WORKDIRS[@]} logs directories for '$PREFIX'"

  # Detect all unique operations (READ, UPDATE, etc.)
  ops=$(find "${WORKDIRS[@]}" -maxdepth 1 -type f -name '*.hdr' \
    | sed -n 's/.*-\([A-Z]\+\)\.hdr/\1/p' | sort -u)

  # Create output directory
  OUTPUT_DIR="${OUTPUT_BASE}/${PREFIX}"
  mkdir -p "$OUTPUT_DIR"

  # Process each operation
  for op in $ops; do
    echo "============================"
    echo "~_~T Processing union for operation: $op"
    echo "============================"

    output_file="${OUTPUT_DIR}/union-${op}${SUFFIX}.hdr"
    args=(union)

    files_found=0
    for dir in "${WORKDIRS[@]}"; do
      for file in "$dir"/*-"$op".hdr; do
        if [ -f "$file" ]; then
          abs_file=$(realpath "$file")
          echo "  ~_~S~N Adding: $abs_file"
          args+=("-ifp" "$abs_file")
          files_found=$((files_found + 1))
        fi
      done
    done

    if [ "$files_found" -eq 0 ]; then
      echo "  ~Z| ~O No input files for $op, skipping."
      continue
    fi

    args+=("-of" "$output_file")

    echo "[DEBUG] Running: java -jar $PROCESSOR_JAR ${args[*]}"
    java -jar "$PROCESSOR_JAR" "${args[@]}"

    if [ -f "$output_file" ]; then
      echo "~\\~E Finished: $output_file"
    else
      echo "~]~L Failed to generate: $output_file"
    fi
    echo
  done
done
