#!/bin/sh
# Builds the module in the local FreeBSD development VM and runs tests/run.sh there.
# The VM (KVM, amd64) is ~/fbsd-vm/disk.qcow2, ssh on port 2222 (key ~/fbsd-vm/id_ed25519); it is started here
# with -snapshot (nothing is written to its disk) when it is not running.  The kernel sources are expected in
# the VM at $SYSDIR (default /tmp/isowork/src/usr/src/sys, the place where wsbsd's iso/build.sh unpacks them).
#   sh tools/vm-test.sh
SYSDIR="${SYSDIR:-/tmp/isowork/src/usr/src/sys}"
VM="$HOME/fbsd-vm"
S="ssh -i $VM/id_ed25519 -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -q root@localhost"
cd "$(dirname "$0")/.." || exit 1

if ! $S true 2>/dev/null; then
	echo "starting the VM (snapshot mode)"
	qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m 4G -snapshot -drive file="$VM/disk.qcow2",if=virtio \
	    -netdev user,id=n0,hostfwd=tcp::2222-:22 -device virtio-net-pci,netdev=n0 -display none \
	    -serial file:"${TMPDIR:-/tmp}/darwinator-vm-serial.log" -daemonize || exit 1
	for i in $(seq 1 60); do $S true 2>/dev/null && break; sleep 3; done
fi
$S 'kldunload darwinator 2>/dev/null; rm -rf /root/darwinator && mkdir -p /root/darwinator' || exit 1
tar cf - --exclude=.git . | $S 'cd /root/darwinator && tar xf -' || exit 1
$S "cd /root/darwinator && make SYSDIR=$SYSDIR >build.log 2>&1 || { tail -30 build.log; exit 1; }; timeout 120 sh tests/run.sh"
