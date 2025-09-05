domain_name="vm1"   # or your $1

# if you want to fully recreate:
sudo virsh destroy "$domain_name" || true
sudo virsh undefine "$domain_name" --nvram || true
sudo rm -rf "/var/lib/libvirt/images/$domain_name"
sudo rm -rf /var/lib/libvirt/images/base/focal-server-cloudimg-amd64.img