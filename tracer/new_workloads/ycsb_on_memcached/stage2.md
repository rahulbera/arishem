# Stage 2: Identifying the ROI and Creating the Golden Snapshot

## Overview

In this stage we:
1. Added a 5th vCPU to isolate the benchmark client from Memcached workers
2. Populated Memcached with ~6 GB of data using YCSB (load phase only)
3. Installed `memtier_benchmark` as a lightweight native C load generator
4. Took a VM snapshot with Memcached warm but idle (no benchmark running)
5. Verified the snapshot by restoring and launching memtier_benchmark

---

## Why This Design

### YCSB for loading, memtier_benchmark for running

YCSB is a Java application. Launching it under TCG (Stage 4) would mean
booting a JVM at 30–50x slowdown — minutes of wall time generating useless
JVM bytecode traces. Instead, we use YCSB only for the one-time data loading
phase (under fast KVM), and switch to `memtier_benchmark` (a native C binary
that starts in milliseconds) for the actual benchmark run phase.

### Snapshot with warm-but-idle Memcached

The snapshot captures Memcached with 2.25 million records (~6 GB) fully
loaded, threads pinned, but no benchmark running. After every restore, the
benchmark starts fresh from the same deterministic state. This means:
- Every trace begins from an identical starting point
- No stale Java NIO connections or timeouts after restore
- In Stage 4, the tracing plugin captures every instruction from the very
  first GET/SET operation

### 5 vCPUs: clean separation

| vCPU | Assigned to | Traced in Stage 4? |
|------|-------------|---------------------|
| 0 | Memcached worker 0 | Yes |
| 1 | Memcached worker 1 | Yes |
| 2 | Memcached worker 2 | Yes |
| 3 | Memcached worker 3 | Yes |
| 4 | memtier_benchmark + OS housekeeping | No |

This ensures traces from vCPUs 0–3 contain only Memcached instructions.

---

## Part 1: Updated Boot Script (5 vCPUs)

On the host:

```bash
cat > ~/qemu-tracing/scripts/boot_kvm_5vcpu.sh << 'BOOT_EOF'
#!/bin/bash
# Boot the guest VM with KVM — 5 vCPUs
# vCPU 0-3: Memcached worker threads
# vCPU 4:   memtier_benchmark + OS housekeeping

IMGDIR="$HOME/qemu-tracing/images"

qemu-system-x86_64 \
    -accel kvm \
    -cpu host \
    -smp 5 \
    -m 12G \
    -drive file=${IMGDIR}/ubuntu-guest.qcow2,format=qcow2,if=virtio \
    -nic user,model=virtio-net-pci,hostfwd=tcp::2222-:22,hostfwd=tcp::11211-:11211 \
    -nographic \
    -serial mon:stdio \
    -monitor telnet:127.0.0.1:4444,server,nowait \
    -qmp tcp:127.0.0.1:4445,server,nowait
BOOT_EOF

chmod +x ~/qemu-tracing/scripts/boot_kvm_5vcpu.sh
```

---

## Part 2: Install memtier_benchmark

SSH into the guest:

```bash
ssh -p 2222 researcher@localhost
```

Try the package manager first:

```bash
sudo apt install -y memtier-benchmark
```

If not available in the repos, build from source:

```bash
sudo apt install -y build-essential autoconf automake libpcre3-dev \
                    libevent-dev pkg-config zlib1g-dev libssl-dev
cd ~
git clone https://github.com/RedisLabs/memtier_benchmark.git
cd memtier_benchmark
autoreconf -ivf
./configure
make -j4
sudo make install
```

Verify installation:

```bash
memtier_benchmark --version
```

---

## Part 3: Guest-Side Scripts

### 3.1 — Experiment script (load phase + snapshot preparation)

```bash
cat << 'EXPEOF' > ~/run_experiment_v2.sh
#!/bin/bash
set -e

# Verify Python 2
PYVER=$(python --version 2>&1 | awk '{print $2}')
if [[ ! "$PYVER" == 2.* ]]; then
    echo "ERROR: Python 2 required but found: $PYVER"
    echo "Activate your conda py2 environment first: conda activate py2"
    exit 1
fi

# ==============================================================
# CONFIGURATION
# ==============================================================
MEMCACHED_MEM=8192          # Memcached memory in MB
MEMCACHED_THREADS=4         # Number of Memcached worker threads
YCSB_DIR="$HOME/ycsb"
RECORD_COUNT=2250000        # ~6 GB data footprint
FIELD_COUNT=10
FIELD_LENGTH=400
YCSB_CLIENT_THREADS=16
YCSB_VCPU=4                # Pin YCSB to this vCPU (away from Memcached)

# ==============================================================
# STEP 1: Start Memcached with thread pinning
# ==============================================================
echo "=========================================="
echo "STEP 1: Starting Memcached"
echo "=========================================="

sudo killall memcached 2>/dev/null || true
sleep 1

sudo /usr/bin/memcached -u memcache -p 11211 \
    -m ${MEMCACHED_MEM} \
    -t ${MEMCACHED_THREADS} \
    -l 0.0.0.0 \
    -C -c 1024 \
    -o hashpower=20 &
sleep 2
MCPID=$(pgrep -x memcached)

TIDS=($(sudo ls /proc/$MCPID/task/))
echo "Memcached PID: $MCPID, Threads: ${#TIDS[@]}"

VCPU=0
for i in "${!TIDS[@]}"; do
    TID=${TIDS[$i]}
    if [ "$i" -ge 1 ] && [ "$VCPU" -lt "$MEMCACHED_THREADS" ]; then
        sudo taskset -p -c $VCPU $TID >/dev/null 2>&1
        echo "  Worker thread $TID → vCPU $VCPU"
        VCPU=$((VCPU + 1))
    fi
done

echo "stats" | nc -q 1 localhost 11211 | grep -E "threads|limit_maxbytes" || {
    echo "ERROR: Memcached not responding!"
    exit 1
}
echo ""

# ==============================================================
# STEP 2: YCSB Load Phase
# ==============================================================
echo "=========================================="
echo "STEP 2: YCSB Load Phase"
echo "  Records: ${RECORD_COUNT}"
echo "  Record size: ~$((FIELD_COUNT * FIELD_LENGTH)) bytes"
echo "  Expected footprint: ~$((RECORD_COUNT * FIELD_COUNT * FIELD_LENGTH / 1024 / 1024 / 1024)) GB"
echo "=========================================="

cd ${YCSB_DIR}

taskset -c ${YCSB_VCPU} ./bin/ycsb load memcached -s \
    -p workload=site.ycsb.workloads.CoreWorkload \
    -p recordcount=${RECORD_COUNT} \
    -p fieldcount=${FIELD_COUNT} \
    -p fieldlength=${FIELD_LENGTH} \
    -p memcached.hosts=127.0.0.1 \
    -p memcached.port=11211 \
    -threads ${YCSB_CLIENT_THREADS}

echo ""
echo "Load phase complete. Verifying data..."

STATS=$(echo "stats" | nc -q 1 localhost 11211)
ITEMS=$(echo "$STATS" | grep "curr_items" | awk '{print $3}' | tr -d '\r')
BYTES=$(echo "$STATS" | grep "bytes " | grep -v "bytes_" | awk '{print $3}' | tr -d '\r')
EVICTIONS=$(echo "$STATS" | grep "evictions" | awk '{print $3}' | tr -cd '0-9')
BYTES_GB=$(echo "scale=2; ${BYTES:-0} / 1024 / 1024 / 1024" | bc)

echo "  Items loaded:  ${ITEMS}"
echo "  Memory used:   ${BYTES_GB} GB"
echo "  Evictions:     ${EVICTIONS}"

if [ "${EVICTIONS:-0}" -gt 0 ]; then
    echo "  WARNING: Evictions occurred! Consider reducing RECORD_COUNT."
fi
echo ""

# ==============================================================
# STEP 3: Quick memtier sanity check
# ==============================================================
echo "=========================================="
echo "STEP 3: Quick memtier_benchmark sanity check (5 seconds)"
echo "=========================================="

taskset -c ${YCSB_VCPU} memtier_benchmark \
    --server=127.0.0.1 \
    --port=11211 \
    --protocol=memcache_text \
    --threads=4 \
    --clients=4 \
    --ratio=1:1 \
    --test-time=5 \
    --key-maximum=${RECORD_COUNT} \
    --key-pattern=G:G \
    --data-size=$((FIELD_COUNT * FIELD_LENGTH)) \
    --hide-histogram

echo ""

# ==============================================================
# STEP 4: Ready for snapshot
# ==============================================================
echo "=========================================="
echo "SNAPSHOT TIME"
echo "=========================================="
echo ""
echo "Memcached is warm with ${ITEMS} items (${BYTES_GB} GB)."
echo "No benchmark is running — clean state for snapshot."
echo ""
echo "Take the snapshot NOW from another terminal:"
echo ""
echo "  telnet localhost 4444"
echo "  savevm roi_ready"
echo "  (then press Ctrl+] and type quit to exit telnet)"
echo ""
echo "Waiting... (press Ctrl+C after you've taken the snapshot)"

while true; do
    sleep 5
    echo "  ... still waiting for snapshot (Memcached is running)"
done
EXPEOF

chmod +x ~/run_experiment_v2.sh
```

### 3.2 — Benchmark script (run after snapshot restore)

```bash
cat << 'BENCHEOF' > ~/start_benchmark.sh
#!/bin/bash
# Run memtier_benchmark against already-running Memcached
# Use after restoring a snapshot (under KVM for verification or TCG for tracing)

# ==============================================================
# CONFIGURATION — edit these to change workload characteristics
# ==============================================================
RATIO="1:1"             # GET:SET ratio. 1:1 = 50/50. 19:1 = 95/5 read-heavy.
KEY_MAX=2250000         # Must match the loaded record count
DATA_SIZE=4000          # Must match field_count * field_length
THREADS=4               # Client threads
CLIENTS=4               # Connections per thread
DURATION=30             # Seconds to run (use large value for tracing)
BENCH_VCPU=4            # Pin to this vCPU (away from Memcached workers)

echo "Starting memtier_benchmark..."
echo "  Ratio (GET:SET): ${RATIO}"
echo "  Key range: 1-${KEY_MAX}"
echo "  Duration: ${DURATION}s"
echo ""

taskset -c ${BENCH_VCPU} memtier_benchmark \
    --server=127.0.0.1 \
    --port=11211 \
    --protocol=memcache_text \
    --threads=${THREADS} \
    --clients=${CLIENTS} \
    --ratio=${RATIO} \
    --test-time=${DURATION} \
    --key-maximum=${KEY_MAX} \
    --key-pattern=G:G \
    --data-size=${DATA_SIZE} \
    --hide-histogram
BENCHEOF

chmod +x ~/start_benchmark.sh
```

---

## Part 4: Executing the Workflow

### Step 4.1 — Boot the VM

On the host (Terminal 1):

```bash
~/qemu-tracing/scripts/boot_kvm_5vcpu.sh
```

### Step 4.2 — Run the experiment

On the host (Terminal 2):

```bash
ssh -p 2222 researcher@localhost
# Activate conda environment for YCSB's Python 2 requirement
conda activate py2
~/run_experiment_v2.sh
```

Wait for the load phase to complete (~several minutes for 2.25M records).
The script will print data verification stats and then wait for you to
take the snapshot.

### Step 4.3 — Take the snapshot

On the host (Terminal 3):

```bash
telnet localhost 4444
```

At the QEMU monitor prompt:

```
(qemu) savevm roi_ready
```

Wait for the prompt to return (a few seconds while it saves ~12 GB of state).

**Exit telnet without killing the VM:**

```
Press Ctrl+]
telnet> quit
```

> **WARNING:** Do NOT type `quit` at the `(qemu)` prompt. That kills the VM.
> Always press `Ctrl+]` first to drop to the telnet prompt, then type `quit`.

### Step 4.4 — Verify the snapshot exists

On the host:

```bash
qemu-img snapshot -l ~/qemu-tracing/images/ubuntu-guest.qcow2
```

Expected output:

```
Snapshot list:
ID  TAG        VM SIZE               DATE                 VM CLOCK     ICOUNT
1   roi_ready  ~7 GiB   2026-04-xx xx:xx:xx  00:xx:xx.xxx
```

The VM SIZE being smaller than 12 GB is normal — QEMU compresses the snapshot.

### Step 4.5 — Shut down the VM

In Terminal 2 (SSH):

```bash
sudo shutdown -h now
```

---

## Part 5: Verify the Snapshot

### Step 5.1 — Restore from snapshot

On the host:

```bash
qemu-system-x86_64 \
    -accel kvm \
    -cpu host \
    -smp 5 \
    -m 12G \
    -drive file=$HOME/qemu-tracing/images/ubuntu-guest.qcow2,format=qcow2,if=virtio \
    -nic user,model=virtio-net-pci,hostfwd=tcp::2222-:22,hostfwd=tcp::11211-:11211 \
    -nographic \
    -serial mon:stdio \
    -monitor telnet:127.0.0.1:4444,server,nowait \
    -loadvm roi_ready
```

The VM does not boot from scratch — it jumps directly to the saved state.

### Step 5.2 — Verify Memcached state

SSH in (give it a few seconds):

```bash
ssh -p 2222 researcher@localhost
```

```bash
# Check data is present
echo "stats" | nc -q 1 localhost 11211 | grep -E "curr_items|bytes |threads"
# Expected: curr_items ~2250000, bytes ~6 GB, threads 4

# Check thread pinning survived the restore
MCPID=$(pgrep -x memcached)
for TID in $(sudo ls /proc/$MCPID/task/); do
    AFF=$(sudo taskset -p $TID 2>/dev/null | awk '{print $NF}')
    echo "Thread $TID affinity mask: $AFF"
done
# Expected: 4 worker threads with masks 1, 2, 4, 8
```

### Step 5.3 — Launch benchmark and verify throughput

```bash
~/start_benchmark.sh
```

Expected: memtier_benchmark starts immediately, reporting ~30K+ ops/sec
for both GETs and SETs. All 4 Memcached worker threads should show
meaningful CPU usage in `top -H`.

### Step 5.4 — Clean shutdown

```bash
sudo shutdown -h now
```

---

## Part 6: Managing Multiple Snapshots

### Creating snapshots for different workload mixes

Repeat the workflow with different `RATIO` values in `start_benchmark.sh`:

| Snapshot tag | RATIO | Description |
|-------------|-------|-------------|
| `roi_ready` | — | Base snapshot (warm data, no benchmark) |
| `roi_read_heavy` | `19:1` | For 95/5 read-heavy studies |
| `roi_write_heavy` | `1:4` | For 20/80 read-write studies |

Note: since the snapshot is taken with Memcached idle, the same `roi_ready`
snapshot works for all ratios — just change `RATIO` in `start_benchmark.sh`
before launching. You only need multiple snapshots if you want different
data footprints or record counts.

### Useful snapshot commands

```bash
# List all snapshots
qemu-img snapshot -l ~/qemu-tracing/images/ubuntu-guest.qcow2

# Delete a snapshot
qemu-img snapshot -d roi_ready ~/qemu-tracing/images/ubuntu-guest.qcow2

# Back up the disk image (includes all snapshots)
cp ~/qemu-tracing/images/ubuntu-guest.qcow2 \
   ~/qemu-tracing/snapshots/ubuntu-guest-roi-ready-backup.qcow2
```

---

## Part 7: Snapshot Metadata

```bash
cat > ~/qemu-tracing/snapshots/roi_ready_metadata.txt << 'META_EOF'
Snapshot: roi_ready
Date: [fill in]
QEMU version: [run: qemu-system-x86_64 --version]

Guest Configuration:
  - Ubuntu Server 24.04, kernel 6.8.0-107-generic
  - 5 vCPUs, 12 GB RAM
  - ASLR disabled, swap off, THP disabled

Memcached Configuration:
  - Memory: 8192 MB allocated, ~6 GB used
  - Worker threads: 4, pinned to vCPUs 0-3
  - Hash power: 20
  - Items: 2,250,000 records (~4 KB each)

Data Loading:
  - Tool: YCSB 0.17.0 (load phase only)
  - Record count: 2,250,000
  - Field count: 10, field length: 400 bytes

Benchmark (launched after restore):
  - Tool: memtier_benchmark (native C, starts instantly)
  - Default ratio: 1:1 (50/50 GET:SET)
  - Key distribution: Gaussian (approximates Zipfian)
  - Observed throughput: ~33K ops/sec GET + ~33K ops/sec SET

Snapshot State:
  - Memcached warm with ~6 GB data, threads pinned, idle (no benchmark)
  - Benchmark is started after restore via ~/start_benchmark.sh

vCPU Mapping (for Stage 4 tracing):
  - vCPU 0: Memcached worker 0 → Socket 0, Core 0
  - vCPU 1: Memcached worker 1 → Socket 0, Core 1
  - vCPU 2: Memcached worker 2 → Socket 1, Core 0
  - vCPU 3: Memcached worker 3 → Socket 1, Core 1
  - vCPU 4: memtier_benchmark + OS (not traced)
META_EOF
```

---

## Directory Structure After Stage 2

```
~/qemu-tracing/
├── images/
│   ├── ubuntu-24.04-server-cloudimg-amd64.iso  (can delete)
│   ├── cloud-init.iso                          (can delete)
│   └── ubuntu-guest.qcow2                      (VM disk + roi_ready snapshot)
├── snapshots/
│   ├── roi_ready_metadata.txt
│   └── ubuntu-guest-roi-ready-backup.qcow2     (optional backup)
├── traces/                                      (for Stage 4)
└── scripts/
    ├── boot_kvm.sh                              (4 vCPU, Stage 1)
    └── boot_kvm_5vcpu.sh                        (5 vCPU, Stage 2+)
```

Guest-side scripts:

```
~/run_experiment_v2.sh      Load data + prepare for snapshot
~/start_benchmark.sh        Launch benchmark after restore
```

---

## Stage 2 Checklist

- [ ] VM boots correctly with 5 vCPUs
- [ ] YCSB load phase populates ~6 GB of data (2.25M records)
- [ ] Zero evictions during loading
- [ ] memtier_benchmark sanity check passes
- [ ] Snapshot `roi_ready` saved successfully
- [ ] Restore with `-loadvm roi_ready` works
- [ ] After restore: Memcached has ~2.25M items, ~6 GB data
- [ ] After restore: Memcached worker threads still pinned to vCPUs 0-3
- [ ] After restore: `~/start_benchmark.sh` shows ~30K+ ops/sec immediately
- [ ] After restore: `top -H` shows all 4 Memcached workers active
- [ ] Snapshot metadata file filled in

**Next: Stage 3 — Writing the TCG tracing plugin.**