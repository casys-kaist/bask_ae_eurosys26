#!/bin/bash
# set -eux -o pipefail

# Modified from Memstrata script

SSH_KEY="/root/.ssh/id_test123"
VM_NET="default"     # libvirt network name
SSH_USER="ubuntu"

get_vm_ip() {
  local name="$1"
  local ip=""

  # 1) Try guest agent (requires qemu-guest-agent in the guest)
  ip=$(sudo virsh domifaddr "$name" --source agent 2>/dev/null \
      | awk '/ipv4/ {print $4}' | sed 's#/.*##' | head -n1)
  [[ -n "$ip" ]] && { echo "$ip"; return 0; }

  # 2) Try libvirt leases
  ip=$(sudo virsh domifaddr "$name" --source lease 2>/dev/null \
      | awk '/ipv4/ {print $4}' | sed 's#/.*##' | head -n1)
  [[ -n "$ip" ]] && { echo "$ip"; return 0; }

  # 3) Match MAC -> DHCP lease table
  local mac
  mac=$(sudo virsh domiflist "$name" 2>/dev/null | awk 'NR>2 && NF {print $5; exit}')
  if [[ -n "$mac" ]]; then
    ip=$(sudo virsh net-dhcp-leases "$VM_NET" 2>/dev/null \
        | awk -v m="$mac" '$0~m {print $5}' | sed 's#/.*##' | head -n1)
    [[ -n "$ip" ]] && { echo "$ip"; return 0; }
  fi

  # 4) ARP/neighbor fallback (needs recent traffic)
  if [[ -n "$mac" ]]; then
    ip=$(ip neigh 2>/dev/null | awk -v m="$mac" '$0~m {print $1; exit}')
    [[ -n "$ip" ]] && { echo "$ip"; return 0; }
    ip=$(arp -an 2>/dev/null | awk -v m="$(echo "$mac" | tr '[:lower:]' '[:upper:]')" \
         '$0~m {gsub(/[()]/,""); print $2; exit}')
    [[ -n "$ip" ]] && { echo "$ip"; return 0; }
  fi

  return 1
}

wait_for_vm_ready() {
  local name="$1"
  local ip=""
  echo "Waiting for IP of $name..."
  for i in {1..60}; do
    ip=$(get_vm_ip "$name") && break
    sleep 2
  done
  [[ -z "$ip" ]] && { echo "Failed to discover IP for $name"; return 1; }
  echo "VM IP: $ip"

  echo "Waiting for SSH to be reachable..."
  until sudo ssh -i "$SSH_KEY" -o IdentitiesOnly=yes \
            -o ConnectTimeout=5 -o StrictHostKeyChecking=no \
            "$SSH_USER@$ip" "true" 2>/dev/null; do
    sleep 3
  done
  echo "SSH reachable."

  echo "Waiting for cloud-init to finish..."
  sudo ssh -i "$SSH_KEY" -o IdentitiesOnly=yes -o StrictHostKeyChecking=no \
      "$SSH_USER@$ip" "cloud-init status --wait" || true
}

sudo apt-get update
sudo apt-get install libusbredirparser-dev libusb-1.0-0-dev ninja-build git libusb-dev libglib2.0-dev libfdt-dev libpixman-1-dev zlib1g-dev libaio-dev libbluetooth-dev libbrlapi-dev libbz2-dev libcap-dev libcap-ng-dev libcap-dev libcap-ng-dev libcurl4-gnutls-dev libgtk-3-dev librbd-dev librdmacm-dev libsasl2-dev libsdl1.2-dev libseccomp-dev libsnappy-dev libssh2-1-dev libvde-dev libvdeplug-dev libxen-dev liblzo2-dev valgrind xfslibs-dev libnfs-dev libiscsi-dev -y
sudo apt update
sudo apt install -y xmlstarlet htop uvtool libvirt-daemon-system libvirt-clients bridge-utils virt-manager libosinfo-bin libguestfs-tools virt-top

sudo xmlstarlet ed --inplace -d '//graphics' /usr/share/uvtool/libvirt/template.xml
sudo xmlstarlet ed --inplace -d '//video' /usr/share/uvtool/libvirt/template.xml

if [ ! -f /var/lib/libvirt/images/base/focal-server-cloudimg-amd64.img ]; then
    if [ ! -f focal-server-cloudimg-amd64.img ]; then
        echo "Downloading the base image..."
        wget https://cloud-images.ubuntu.com/focal/current/focal-server-cloudimg-amd64.img
    fi
    sudo mkdir /var/lib/libvirt/images/base -p
    sudo cp focal-server-cloudimg-amd64.img /var/lib/libvirt/images/base/
fi

domain_name="vm1"   # or your $1


sudo mkdir -p /var/lib/libvirt/images/$domain_name
sudo qemu-img create -f qcow2 -F qcow2 -o \
    backing_file=/var/lib/libvirt/images/base/focal-server-cloudimg-amd64.img \
    /var/lib/libvirt/images/$domain_name/$domain_name.qcow2
sudo qemu-img resize /var/lib/libvirt/images/$domain_name/$domain_name.qcow2 225G

cd /tmp
cat >meta-data <<EOF
local-hostname: $domain_name
EOF

export PUB_KEY=$(sudo cat /root/.ssh/id_test123.pub)
if [ -z "$PUB_KEY" ]; then
    echo "Cannot find the public key of root"
    exit 1
fi

cat >user-data <<EOF
#cloud-config
ssh_pwauth: true

users:
  - name: ubuntu
    groups: [sudo]
    shell: /bin/bash
    sudo: ALL=(ALL) NOPASSWD:ALL
    lock_passwd: false
    ssh_authorized_keys:
      - ${PUB_KEY}

chpasswd:
  list: |
    ubuntu:12345678
  expire: False

runcmd:
  - sed -i 's/^#\?PasswordAuthentication.*/PasswordAuthentication yes/' /etc/ssh/sshd_config
  - echo "AllowUsers ubuntu" >> /etc/ssh/sshd_config
  - systemctl restart ssh
EOF

sudo genisoimage -output /var/lib/libvirt/images/$domain_name/$domain_name-cidata.iso -volid cidata -joliet -rock user-data meta-data

sudo virt-install --connect qemu:///system --virt-type kvm --name $domain_name \
    --ram $((128 * 1024)) --vcpus=32 --os-variant ubuntu20.04 \
    --disk path=/var/lib/libvirt/images/$domain_name/$domain_name.qcow2,format=qcow2 \
    --disk /var/lib/libvirt/images/$domain_name/$domain_name-cidata.iso,device=cdrom \
    --import --network network=default --noautoconsole

wait_for_vm_ready "$domain_name"

# Disable cache for the disk
sudo virsh dumpxml $domain_name > /tmp/$domain_name.xml
sudo xmlstarlet ed --inplace -s "/domain/devices/disk/driver" -t attr -n "cache" -v "none" /tmp/$domain_name.xml
sudo virsh define /tmp/$domain_name.xml --validate

sudo virsh reboot $domain_name
wait_for_vm_ready "$domain_name"
