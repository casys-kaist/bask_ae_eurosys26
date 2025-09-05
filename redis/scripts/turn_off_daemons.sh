#!/bin/bash

# Services to temporarily stop for benchmarking
SERVICES=(
  multipathd
  snapd
  docker
  containerd
  polkit
  ssh
)

echo "Stopping unnecessary services for this session only..."

for SERVICE in "${SERVICES[@]}"; do
  echo "-> Stopping $SERVICE.service"
  systemctl stop "$SERVICE".service 2>/dev/null
done

echo "All specified services have been stopped (not disabled)."
echo "They will start automatically on the next reboot."

