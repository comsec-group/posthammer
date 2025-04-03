#! /bin/bash

set -ux

MOUNTPOINT=/mnt/huge/

nix_sudo() {
	sudo --preserve-env=PATH,PYTHONPATH,PWD env "$@"
}

mount_hugetlb() {
	local uid=$1

	# See Documentation/admin-guide/mm/hugetlbpage.rst
	sudo umount -t hugetlbfs $MOUNTPOINT/1g
	sudo mkdir -p $MOUNTPOINT/1g
	echo 1 | sudo tee /sys/devices/system/node/node0/hugepages/hugepages-1048576kB/nr_hugepages
	sudo mount -t hugetlbfs -o pagesize=1GB,min_size=1GB,size=1G none $MOUNTPOINT/1g

	sudo umount -t hugetlbfs $MOUNTPOINT/2m
	sudo mkdir -p $MOUNTPOINT/2m
	echo 512 | sudo tee /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
	sudo mount -t hugetlbfs -o pagesize=2M,min_size=1GB,size=1G none $MOUNTPOINT/2m

	sudo chown -R $uid $MOUNTPOINT
}

setup_perf_counters() {
	# For now, we just need LCC misses. We use IA_32PMC0, see 19.2.1.1
	echo 2 | sudo tee /sys/devices/cpu/rdpmc

	# Configure the IA_PERFEVTSEL0 reg (0x186) (associated with IA_32PMC0)
	# for tracking LLC misses (Table 19-1):
	#	Event select: 0x2e
	#	UMask: 0x41
	#	USR flag: 0x1
	#	OS flag: 0x0
	#	Edge detect: 0x0
	#	PC flag: reserved (0x0)
	#	INT flag: 0x0 (interrupt on overflow: big delay)
	#	EN flag: 0x1 (enable the counter)
	#	INV flag: 0x0
	#	Counter mask: 0x0
	#
	#	We get: 0x51412e for enable
	sudo modprobe msr
	sudo wrmsr -a 0x186 0x41412e
}

# We need this to find eviction sets reliably
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
mount_hugetlb $(id -u)
setup_perf_counters

# To run DRAMA:
# cd drama && make && sudo ./drama $(hostname) a0 manufacturer

# To run the fuzzer:
cd pattern && make && sudo ./pattern ../drama/bank-stats/a2.csv dummy null $(ls ../snapshot)
