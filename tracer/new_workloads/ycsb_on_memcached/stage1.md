# Stage 1: Setting Up QEMU + Memcached + YCSB

## Your Setup
- **Host:** Intel i7-8700 (6C/12T), 32 GB RAM, Ubuntu 24.04
- **Guest:** Ubuntu Server 24.04, 4 vCPUs, 12 GB RAM, 40 GB disk
- **Goal:** Run YCSB → Memcached inside the guest VM with KVM acceleration

---

## Part 1: Install QEMU with KVM on the Host

### Step 1.1 — Verify KVM support

Your i7-8700 supports Intel VT-x. Verify it's enabled:

```bash
# Check if KVM modules are loaded
lsmod | grep kvm

# You should see:
#   kvm_intel    ...
#   kvm          ...

# If you see nothing, enable VT-x in BIOS first, then:
sudo modprobe kvm_intel
```

Verify with:

```bash
ls -la /dev/kvm
# Should show: crw-rw---- 1 root kvm ...
```

### Step 1.2 — Install QEMU and dependencies

```bash
sudo apt update
sudo apt install -y qemu-system-x86 qemu-utils ovmf virt-manager \
                    libvirt-daemon-system bridge-utils cpu-checker

# Verify KVM is working
sudo kvm-ok
# Should print: "KVM acceleration can be used"
```

### Step 1.3 — Add yourself to the kvm group

```bash
sudo usermod -aG kvm $USER
# IMPORTANT: Log out and log back in for this to take effect
# Or run: newgrp kvm
```

### Step 1.4 — Create a working directory

```bash
mkdir -p ~/qemu-tracing/{images,snapshots,traces,scripts}
cd ~/qemu-tracing
```

---

## Part 2: Create a Guest VM

> Part 2a.XX provides instructions to setup Ubuntu Server using `cloud-init`, whereas Part 2b.XX provides instructions to setup the same using the native GUI. 2a is useful only if you are creating VM on a host machine that you can only access via SSH. If you have physical/GUI access to the host machine, use instructions from Part 2b. Instructions from 2.6 onwards are same for both the methods.


### Step 2a.1 — Download Ubuntu 24.04 cloud image

```bash
cd ~/qemu-tracing/images

# Download the Ubuntu 24.04 cloud image (pre-installed, ~600 MB)
wget https://cloud-images.ubuntu.com/releases/24.04/release/ubuntu-24.04-server-cloudimg-amd64.img

# Resize it to 40 GB (cloud images ship as ~3 GB minimal)
qemu-img resize ubuntu-24.04-server-cloudimg-amd64.img 40G

# Rename for clarity
mv ubuntu-24.04-server-cloudimg-amd64.img ubuntu-guest.qcow2
```

### Step 2a.2 -- Setting up cloud-init

```bash
# Install cloud-init tools on the host
sudo apt install -y cloud-image-utils

# Create the cloud-init user-data config
cat > ~/qemu-tracing/images/user-data.yaml << 'EOF'
#cloud-config
hostname: qemu-guest
manage_etc_hosts: true

users:
  - name: researcher
    sudo: ALL=(ALL) NOPASSWD:ALL
    shell: /bin/bash
    lock_passwd: false
    # Password is "research123" — change this to whatever you want
    # Generate your own hash with: mkpasswd --method=SHA-512
    passwd: $6$rounds=4096$randomsalt$VO4uMPLmYBGEMVJh8T6U5ZpGDDQH6MIb.bLmOvFMDndN1aXtQpjH8ZKJ.VPzBqXOsLJNqRzgGhJBqCMNuD.h0

ssh_pwauth: true

# Grow the root partition to use all 40 GB
growpart:
  mode: auto
  devices: ['/']

# Install SSH server
packages:
  - openssh-server

runcmd:
  - systemctl enable ssh
  - systemctl start ssh
EOF

# Create the cloud-init ISO (QEMU will attach this as a virtual CD-ROM)
cloud-localds ~/qemu-tracing/images/cloud-init.iso ~/qemu-tracing/images/user-data.yaml
```

### Step 2a.3 -- Boot first time with cloud-init disk attached

```bash
qemu-system-x86_64 \
    -accel kvm \
    -cpu host \
    -smp 4 \
    -m 12G \
    -drive file=$HOME/qemu-tracing/images/ubuntu-guest.qcow2,format=qcow2,if=virtio \
    -drive file=$HOME/qemu-tracing/images/cloud-init.iso,format=raw,if=virtio \
    -nic user,model=virtio-net-pci,hostfwd=tcp::2222-:22,hostfwd=tcp::11211-:11211 \
    -nographic \
    -serial mon:stdio
```

### Step 2a.4 -- Confirm the boot. isable the cloud-init drive from future boots and shut down the VM.

```bash
# Open a new terminal and SSH
ssh -p 2222 researcher@localhost
# A successful SSH connection means you have the VM up and running

# Now while SSHed in, disable the cloud-init services first
sudo touch /etc/cloud/cloud-init.disabled
sudo systemctl disable cloud-init cloud-init-local cloud-config cloud-final

# Then shotdown the VM
sudo shutdown -h now
```

The VM setup is done. Please move to Step 2.5. 


### Step 2b.1 — Download Ubuntu Server 24.04 ISO

```bash
cd ~/qemu-tracing/images

# Download Ubuntu Server 24.04.x LTS (minimal, no desktop — we don't need GUI)
wget https://releases.ubuntu.com/24.04/ubuntu-24.04.2-live-server-amd64.iso
```

> **Why Server and not Desktop?** The server image has no graphical desktop,
> which means less noise in your traces (no window manager, no compositor,
> no display server running). Every CPU cycle in the guest should be doing
> useful work or OS housekeeping, not rendering pixels.

### Step 2b.2 — Create a virtual disk image

```bash
# Create a 40 GB qcow2 disk image
# qcow2 is copy-on-write: it starts small on disk and grows as you write to it
qemu-img create -f qcow2 ~/qemu-tracing/images/ubuntu-guest.qcow2 40G
```

### Step 2b.3 — Install Ubuntu Server into the VM

This will boot the Ubuntu installer inside QEMU. You'll interact with it
via a VNC or graphical window.

```bash
qemu-system-x86_64 \
    -accel kvm \
    -cpu host \
    -smp 4 \
    -m 12G \
    -drive file=$HOME/qemu-tracing/images/ubuntu-guest.qcow2,format=qcow2,if=virtio \
    -cdrom $HOME/qemu-tracing/images/ubuntu-24.04.2-live-server-amd64.iso \
    -boot d \
    -nic user,model=virtio-net-pci,hostfwd=tcp::2222-:22,hostfwd=tcp::11211-:11211 \
    -display gtk \
    -vga virtio
```

**What each flag means:**

| Flag | Purpose |
|------|---------|
| `-accel kvm` | Use KVM hardware virtualization (near-native speed) |
| `-cpu host` | Pass through the host CPU model to the guest |
| `-smp 4` | 4 virtual CPUs |
| `-m 12G` | 12 GB guest RAM (8 GB for Memcached + 4 GB for OS/YCSB/Java) |
| `-drive ...` | Attach the qcow2 disk as a virtio block device |
| `-cdrom ...` | Mount the Ubuntu ISO as a CD-ROM for installation |
| `-boot d` | Boot from CD-ROM first |
| `-nic user,...` | User-mode networking with port forwarding (see below) |
| `-display gtk` | Show a graphical window for the VM console |
| `-vga virtio` | Use virtio GPU (fast paravirtualized display) |

**About the port forwarding:**
- `hostfwd=tcp::2222-:22` — SSH: connect to `localhost:2222` on host → port 22 in guest
- `hostfwd=tcp::11211-:11211` — Memcached: connect to `localhost:11211` on host → port 11211 in guest

> **Note on user-mode networking:** QEMU's user-mode (SLIRP) networking is the
> simplest option and requires no host configuration. The guest can access the
> internet (for apt, wget) through NAT. The host can reach the guest via the
> forwarded ports. For our purposes this is sufficient. Bridge networking is
> faster but more complex to set up and unnecessary for trace collection.

### Step 2b.4 — Walk through the Ubuntu Server installer

A graphical window will open showing the Ubuntu installer. Walk through it:

1. **Language:** English
2. **Keyboard:** Your layout
3. **Installation type:** "Ubuntu Server" (not minimized)
4. **Network:** It should auto-detect and get an IP via QEMU's DHCP. Leave defaults.
5. **Proxy:** Leave blank
6. **Mirror:** Leave default
7. **Storage:** Use entire disk, defaults are fine. Confirm destructive action.
8. **Profile setup:**
   - Your name: `researcher` (or whatever you prefer)
   - Server name: `qemu-guest`
   - Username: `researcher`
   - Password: choose something simple, you'll be typing it a lot
9. **SSH:** Check "Install OpenSSH server" — this is important!
10. **Featured snaps:** Don't select any. We'll install what we need with apt.
11. Let it install. This takes 5-10 minutes.
12. When it says "Reboot Now", do it.

After reboot, QEMU may try to boot from the CD-ROM again. If it does,
close the QEMU window (this kills the VM) and proceed to Step 2.5.

### Step 2.5 — Boot the installed VM (no CD-ROM)

From now on, this is your standard boot command. Save it as a script:

```bash
cat > ~/qemu-tracing/scripts/boot_kvm.sh << 'EOF'
#!/bin/bash
# Boot the guest VM with KVM acceleration
# Used for: setup, warmup, fast-forwarding to ROI

IMGDIR="$HOME/qemu-tracing/images"

qemu-system-x86_64 \
    -accel kvm \
    -cpu host \
    -smp 4 \
    -m 12G \
    -drive file=${IMGDIR}/ubuntu-guest.qcow2,format=qcow2,if=virtio \
    -nic user,model=virtio-net-pci,hostfwd=tcp::2222-:22,hostfwd=tcp::11211-:11211 \
    -nographic \
    -serial mon:stdio \
    -monitor telnet:127.0.0.1:4444,server,nowait \
    -qmp tcp:127.0.0.1:4445,server,nowait
EOF

chmod +x ~/qemu-tracing/scripts/boot_kvm.sh
```

**Changes from the install command:**
- Removed `-cdrom` and `-boot d` (no installer ISO)
- Replaced `-display gtk -vga virtio` with `-nographic -serial mon:stdio` (console in your terminal — no GUI needed anymore)
- Added `-monitor telnet:...` — QEMU monitor accessible via `telnet localhost 4444` (we'll use this for snapshots later)
- Added `-qmp tcp:...` — QEMU Machine Protocol for programmatic control

**Boot it:**

```bash
~/qemu-tracing/scripts/boot_kvm.sh
```

You should see the guest's boot messages scroll by in your terminal, followed
by a login prompt. Log in with the credentials you set during installation.

> **How to SSH instead:** Open a second terminal on the host and run:
> ```bash
> ssh -p 2222 researcher@localhost
> ```
> SSH is much more convenient than the serial console — you get proper terminal
> handling, copy-paste, etc. Use SSH for all remaining steps.

### Step 2.6 — Initial guest configuration

SSH into the guest and do some housekeeping:

```bash
# From a new terminal on the host:
ssh -p 2222 researcher@localhost

# --- Inside the guest ---

# Update packages
sudo apt update && sudo apt upgrade -y

# Install essential tools
sudo apt install -y build-essential htop numactl sysstat \
                    linux-tools-common linux-tools-generic

# Disable unnecessary services that would pollute traces
sudo systemctl disable --now snapd snapd.socket snapd.seeded
sudo systemctl disable --now unattended-upgrades
sudo systemctl disable --now apt-daily.timer apt-daily-upgrade.timer
sudo systemctl disable --now ModemManager
sudo systemctl disable --now multipathd

# Disable swap (we don't want the guest swapping during tracing)
sudo swapoff -a
# Also comment out the swap line in /etc/fstab to make it permanent:
sudo sed -i '/swap/s/^/#/' /etc/fstab

# Disable ASLR for reproducible traces
# (Same virtual address → same trace across runs)
echo 'kernel.randomize_va_space=0' | sudo tee /etc/sysctl.d/99-no-aslr.conf
sudo sysctl -p /etc/sysctl.d/99-no-aslr.conf

# Disable transparent huge pages (THP)
# THP causes non-deterministic memory layout changes
echo 'never' | sudo tee /sys/kernel/mm/transparent_hugepage/enabled
echo 'never' | sudo tee /sys/kernel/mm/transparent_hugepage/defrag
# Make permanent via rc.local or a systemd service (shown below)
cat << 'THPEOF' | sudo tee /etc/systemd/system/disable-thp.service
[Unit]
Description=Disable Transparent Huge Pages
DefaultDependencies=no
After=sysinit.target local-fs.target
Before=basic.target

[Service]
Type=oneshot
ExecStart=/bin/sh -c "echo never > /sys/kernel/mm/transparent_hugepage/enabled && echo never > /sys/kernel/mm/transparent_hugepage/defrag"

[Install]
WantedBy=basic.target
THPEOF

sudo systemctl daemon-reload
sudo systemctl enable disable-thp.service
```

---

## Part 3: Install and Configure Memcached

### Step 3.1 — Install Memcached

```bash
# --- Inside the guest (via SSH) ---

sudo apt install -y memcached libmemcached-tools
```

### Step 3.2 — Configure Memcached for our experiment

We need Memcached configured with 4 worker threads and 8 GB of memory.

```bash
# Back up the default config
sudo cp /etc/memcached.conf /etc/memcached.conf.bak

# Write our custom config
cat << 'MCEOF' | sudo tee /etc/memcached.conf
# Listen on all interfaces (needed for YCSB to connect)
-l 0.0.0.0

# Port
-p 11211

# Run as memcache user
-u memcache

# ========== KEY SETTINGS FOR OUR EXPERIMENT ==========

# 8 GB of memory for caching
-m 8192

# 4 worker threads (one per vCPU)
-t 4

# Maximum item size: 1 MB (default)
-I 1m

# Disable CAS (compare-and-swap) to reduce per-item overhead
# This gives us a slightly denser hash table
-C

# Increase max connections
-c 1024

# Use larger hash table power (2^20 = 1M buckets)
# This reduces hash collisions with a large dataset
-o hashpower=20
MCEOF
```

### Step 3.3 — Pin Memcached threads to specific vCPUs

We need to ensure each Memcached worker thread stays on its assigned vCPU
so that our per-vCPU traces map cleanly to specific threads.

Memcached itself doesn't have a built-in CPU pinning option, so we'll use a
systemd override to launch it with `taskset` and then pin individual threads.

First, let's create a wrapper script that starts Memcached and pins its threads:

```bash
cat << 'PINEOF' | sudo tee /usr/local/bin/start_memcached_pinned.sh
#!/bin/bash
# Start Memcached and pin its worker threads to vCPUs 0-3

# Start Memcached in the background
/usr/bin/memcached -u memcache -p 11211 -m 8192 -t 4 -l 0.0.0.0 \
    -C -c 1024 -o hashpower=20 &
MCPID=$!

echo "Memcached started with PID: $MCPID"

# Wait for threads to spawn
sleep 2

# Get all Memcached thread TIDs
# The main thread + 4 worker threads should appear
TIDS=($(ls /proc/$MCPID/task/))

echo "Found ${#TIDS[@]} threads: ${TIDS[*]}"

# Pin worker threads to vCPUs 0-3
# Thread 0 is the main/listener thread, threads 1-4 are workers
VCPU=0
for i in "${!TIDS[@]}"; do
    TID=${TIDS[$i]}
    if [ "$i" -ge 1 ] && [ "$VCPU" -le 3 ]; then
        taskset -p -c $VCPU $TID
        echo "Pinned worker thread $TID to vCPU $VCPU"
        VCPU=$((VCPU + 1))
    else
        echo "Thread $TID (main/listener) — not pinned"
    fi
done

# Keep running in foreground
wait $MCPID
PINEOF

sudo chmod +x /usr/local/bin/start_memcached_pinned.sh
```

### Step 3.4 — Test Memcached

```bash
# Stop the default Memcached service (if it auto-started)
sudo systemctl stop memcached
sudo systemctl disable memcached

# Start Memcached with our pinning script
sudo /usr/local/bin/start_memcached_pinned.sh &

# Wait a moment, then verify it's running
sleep 3
echo "stats" | nc localhost 11211

# You should see a stats dump. Look for:
#   STAT threads 4
#   STAT limit_maxbytes 8589934592    (= 8 GB)

# Also verify threads are pinned
MCPID=$(pgrep -x memcached)
echo "Memcached PID: $MCPID"

# Check if PID was found
if [ -z "$MCPID" ]; then
    echo "Memcached not running!"
else
    for TID in $(sudo ls /proc/$MCPID/task/); do
        AFF=$(sudo taskset -p $TID 2>/dev/null | awk '{print $NF}')
        echo "Thread $TID affinity mask: $AFF"
    done
fi

# Kill it for now (we'll restart properly later)
sudo killall memcached
```

---

## Part 4: Install and Configure YCSB

### Step 4.1 — Install Java (required by YCSB)

```bash
# --- Inside the guest ---

# YCSB requires Java 8+
sudo apt install -y default-jdk

# Verify
java -version
# Should show OpenJDK 21.x or similar
```

### Step 4.2 — Download YCSB

```bash
cd ~

# Download the latest YCSB release
# (Check https://github.com/brianfrankcooper/YCSB/releases for latest version)
curl -O --location https://github.com/brianfrankcooper/YCSB/releases/download/0.17.0/ycsb-0.17.0.tar.gz

tar xfz ycsb-0.17.0.tar.gz
mv ycsb-0.17.0 ycsb
cd ycsb

# YCSB requires Python2. You can download and install Python2 using minoconda before moving to the next steps

# Verify YCSB is working
./bin/ycsb --help
# Should print usage information

```

> **Note:** YCSB 0.17.0 ships with a Memcached binding out of the box.
> The binding uses the `spymemcached` Java client library, which is included
> in the distribution.

### Step 4.3 — Create YCSB workload configuration files

We'll create two workload files: one for the **load phase** (populating the
database) and one for the **run phase** (the actual benchmark / our ROI).

```bash
# Load phase workload: insert all records
cat << 'LOADEOF' > ~/ycsb/workloads/workload_memcached_load
# Workload for LOADING data into Memcached
# Goal: populate ~8 GB of data

# Use a uniform distribution for loading
# (every record gets inserted exactly once)
recordcount=2000000
operationcount=0

# Field configuration
# Each record has 10 fields, each 400 bytes → ~4 KB per record
# 2,000,000 records × 4 KB ≈ 8 GB
fieldcount=10
fieldlength=400

# Insert all records
workload=site.ycsb.workloads.CoreWorkload
insertstart=0
insertcount=2000000
LOADEOF

# Run phase workload: mixed read/write operations
cat << 'RUNEOF' > ~/ycsb/workloads/workload_memcached_run
# Workload for the RUN phase (our region of interest)
# Based on Workload A (50/50 read/write) for maximum sharing visibility

recordcount=2000000
operationcount=5000000

fieldcount=10
fieldlength=400

workload=site.ycsb.workloads.CoreWorkload

# 50% reads, 50% updates — this maximizes cross-thread data sharing
# because threads will both read and write the same keys
readproportion=0.5
updateproportion=0.5
scanproportion=0
insertproportion=0

# Zipfian distribution: some keys are "hot" and accessed frequently
# This creates realistic sharing patterns where multiple threads
# compete for the same popular cache lines
requestdistribution=zipfian
RUNEOF
```

**Why these specific parameters?**

The math on the data footprint: 2,000,000 records × 10 fields × 400 bytes = 8 GB.
Memcached adds per-item overhead (~80-100 bytes per item for metadata), so the
actual memory consumed will be slightly more than 8 GB, but Memcached's `-m 8192`
will cap it and start evicting if needed. We may need to adjust `recordcount` down
a bit if Memcached runs out of memory during loading. I'd suggest starting with
1,500,000 records (~6 GB) for the first test and then scaling up.

The Zipfian distribution is crucial: it ensures that a small set of "hot" keys
are accessed by all worker threads, creating the cross-thread sharing patterns
that matter for your NUMA study. With uniform random access, sharing would be
rare because the 8 GB address space is large relative to any cache.

### Step 4.4 — First test run: Load phase

```bash
# --- Inside the guest ---

# Start Memcached with thread pinning
sudo /usr/local/bin/start_memcached_pinned.sh &
sleep 3

# Run the YCSB load phase
# This will take several minutes as it inserts 2M records
cd ~/ycsb

./bin/ycsb load memcached -s \
    -P workloads/workload_memcached_load \
    -p memcached.hosts=127.0.0.1 \
    -p memcached.port=11211 \
    -threads 4

# -s        : print status every 10 seconds
# -threads 4: use 4 YCSB client threads (to speed up loading)
```

**What to watch for:**

- You should see progress updates every 10 seconds showing insert throughput
- If you see errors about "SERVER_ERROR out of memory", reduce `recordcount`
- At the end, YCSB prints a summary with throughput and latency stats
- Typical load throughput under KVM: 30,000-80,000 ops/sec

Verify the data is loaded:

```bash
echo "stats" | nc localhost 11211 | grep -E "curr_items|bytes "
# curr_items should be close to 2,000,000 (or your recordcount)
# bytes should be close to 8 GB
```

### Step 4.5 — First test run: Run phase

```bash
# Run the YCSB benchmark phase
cd ~/ycsb

./bin/ycsb run memcached -s \
    -P workloads/workload_memcached_run \
    -p memcached.hosts=127.0.0.1 \
    -p memcached.port=11211 \
    -threads 4

# This will execute 5,000,000 operations (mix of reads and updates)
# with 4 client threads driving 4 Memcached worker threads
```

**What to watch for:**

- Throughput should be healthy (tens of thousands of ops/sec under KVM)
- Latency should be low (sub-millisecond for most operations)
- Both READ and UPDATE operations should show in the summary
- If you see "0 operations" for a type, check the workload file proportions

### Step 4.6 — Verify multi-threading is actually happening

This is important: you need to confirm that Memcached's worker threads are
all actively processing requests, not just the main thread.

```bash
# While YCSB is running (start it in background with & if needed),
# check per-thread CPU usage:
MCPID=$(pgrep memcached)
top -H -p $MCPID -bn 1 | head -20

# You should see 4 worker threads each consuming significant CPU
# If only 1 thread is busy, the load isn't being distributed

# Also check thread pinning is intact:
for TID in $(ls /proc/$MCPID/task/); do
    CPU=$(cat /proc/$TID/stat | awk '{print $39}')
    AFF=$(taskset -p $TID 2>/dev/null | awk '{print $NF}')
    echo "Thread $TID on CPU $CPU, affinity mask: $AFF"
done
```

---

## Part 5: Operational Notes

### Shutting down cleanly

From the QEMU serial console (or SSH):

```bash
sudo shutdown -h now
```

Or from the QEMU monitor (telnet to port 4444 from the host):

```bash
telnet localhost 4444
# At the (qemu) prompt:
quit
```

### Resuming work

```bash
~/qemu-tracing/scripts/boot_kvm.sh
# Wait for boot, then SSH in:
ssh -p 2222 researcher@localhost
```

### If something goes wrong with the disk image

The qcow2 format supports snapshots. Before making risky changes:

```bash
# From the host (VM must be off):
qemu-img snapshot -c "clean_install" ~/qemu-tracing/images/ubuntu-guest.qcow2

# To revert:
qemu-img snapshot -a "clean_install" ~/qemu-tracing/images/ubuntu-guest.qcow2

# List snapshots:
qemu-img snapshot -l ~/qemu-tracing/images/ubuntu-guest.qcow2
```

### Directory structure after Stage 1

```
~/qemu-tracing/
├── images/
│   ├── ubuntu-24.04.2-live-server-amd64.iso   (installer, can delete later)
│   └── ubuntu-guest.qcow2                     (your VM disk)
├── snapshots/                                  (for Stage 2)
├── traces/                                     (for Stage 4)
└── scripts/
    └── boot_kvm.sh                             (KVM boot script)
```

---

## Troubleshooting

**"Could not access KVM kernel module: Permission denied"**
→ Run `sudo usermod -aG kvm $USER` and log out/in.

**Guest can't reach the internet (apt fails)**
→ User-mode networking should provide NAT automatically. Inside the guest,
try `ping 8.8.8.8`. If it works but DNS fails, add `nameserver 8.8.8.8` to
`/etc/resolv.conf` inside the guest.

**SSH connection refused on port 2222**
→ Make sure the VM is booted and SSH is running inside the guest:
`sudo systemctl status ssh` inside the guest console.

**YCSB "Connection refused" to Memcached**
→ Make sure Memcached is running: `pgrep memcached`. Make sure it's listening
on 0.0.0.0 (not just 127.0.0.1): `ss -tlnp | grep 11211`.

**YCSB is very slow during load phase**
→ This is normal under user-mode networking. YCSB connects over loopback
inside the guest, so networking overhead is minimal. If it's under 1,000 ops/sec,
check if the guest is swapping (`free -h`) or if Memcached is running
out of memory (`echo "stats" | nc localhost 11211 | grep evictions`).

**Only 1 Memcached thread is busy**
→ Memcached distributes connections across worker threads. With only 4 YCSB
client threads, distribution might be uneven. Try increasing YCSB threads
to 8 or 16: `-threads 16`. The 16 YCSB threads will be spread across
Memcached's 4 worker threads more evenly. You can also try passing
`-p memcached.hosts=127.0.0.1` multiple times or opening more connections.

---

## Stage 1 Checklist

Before moving to Stage 2, verify all of the following:

- [ ] QEMU VM boots successfully with KVM (`-accel kvm`)
- [ ] You can SSH into the guest from the host (`ssh -p 2222 researcher@localhost`)
- [ ] Memcached starts with 4 threads and 8 GB memory allocation
- [ ] Memcached worker threads are pinned to vCPUs 0-3
- [ ] YCSB load phase completes and populates ~6-8 GB of data
- [ ] YCSB run phase completes with both READs and UPDATEs
- [ ] `top -H` shows multiple Memcached threads active during YCSB run
- [ ] ASLR is disabled, swap is off, THP is disabled

Once all boxes are checked, you're ready for **Stage 2: Identifying the ROI
and creating the golden snapshot**.