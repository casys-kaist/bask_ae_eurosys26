#!/bin/bash

BASE_IMAGE="base.qcow2"
CLONE_PREFIX="vm"
NUM_CLONES=16  # Change this to how many clones you want

for i in $(seq 1 $NUM_CLONES); do
  CLONE_NAME="${CLONE_PREFIX}${i}.qcow2"
  echo "[*] Creating $CLONE_NAME linked to $BASE_IMAGE..."
  qemu-img create -f qcow2 -F qcow2 -b "$BASE_IMAGE" "$CLONE_NAME"
done

echo "✅ Done creating $NUM_CLONES linked clones."
