# Subaru Telescope Vibration Control System

Real-time acquisition, shared-memory streaming, FITS logging, and analysis tools for characterizing mechanical vibrations at the Subaru Telescope and supporting future vibration correction in SCExAO.

## Overview

Mechanical vibrations from the telescope drive system, encoders, wind loading, and other mechanisms can degrade high-angular-resolution observations. This project measures those vibrations with accelerometers, publishes synchronized acceleration samples to a `milk`/ImageStreamIO shared-memory stream, records the stream as FITS cubes, and provides tools for continuity and power spectral density analysis.

The current system:

- acquires two USB audio devices concurrently through PortAudio;
- samples three accelerometer axes at a nominal **8 kHz per axis**;
- matches samples from the two signal conditioners by ADC timestamp;
- publishes one complete synchronized X/Y/Z frame to the `accel` ImageStreamIO stream;
- logs `accel` to FITS cubes with `milk-streamFITSlog`;
- records callback, timing, and data-continuity diagnostics; and
- generates chronological manifests and time-frequency PSD plots.

Background: [Lozi et al., “Characterizing vibrations at the Subaru Telescope for the Subaru coronagraphic extreme adaptive optics instrument”](https://arxiv.org/pdf/1809.08296).

## System Architecture

```text
PCB accelerometers
  X and Y                  Z
     |                     |
     v                     v
PCB 485B39 SC0        PCB 485B39 SC1
  CH1 = X                CH1 = Z
  CH2 = Y                CH2 unused
     |                     |
     +---- PortAudio callbacks ----+
                                   |
                           per-device queues
                                   |
                       timestamp-matching publisher
                                   |
                                   v
                    ImageStreamIO stream: accel
                         shape: [2, 2, 1]
                                   |
                  +----------------+----------------+
                  |                                 |
                  v                                 v
       milk-streamFITSlog (local)         live SHM consumers / (stream)
       FITS + timing files                milk-nettransmit
                  |
                  v
     manifest, continuity, and PSD analysis
```

Each published frame uses the following layout:

| Stream location | Signal | Physical axis |
|---|---|---|
| `data[0, 0]` | `SC0_CH1` | X / elevation |
| `data[0, 1]` | `SC0_CH2` | Y / azimuth |
| `data[1, 0]` | `SC1_CH1` | Z / optical axis |
| `data[1, 1]` | unused | — |


## Hardware

The current deployment uses:

- 3 PCB piezoelectric accelerometers;
- 2 PCB/The Modal Shop 485B39 two-channel ICP USB signal conditioners;
- 1 Linux acquisition computer;
- Ethernet for remote stream transfer and system access.

The 485B39 presents itself as a USB Class 1 audio device and supports an 8 kHz sample rate. The acquisition code selects each conditioner by its USB device name/serial string rather than by a potentially changing ALSA card number.

## Software Stack

- Linux, tested on Ubuntu Server with a PREEMPT_RT kernel
- GCC and Make
- PortAudio
- `milk`
- ImageStreamIO
- Python 3 and the packages in `requirements.txt`

## Important Machine-Specific Configuration

Review these values before compiling or running on another computer:

| File | Setting |
|---|---|
| `src/paRead.c` | Signal-conditioner serial strings |
| `src/paRead.c` | Accelerometer calibration values |
| `src/paRead.c` | SC0 and SC1 callback CPU affinity |
| `scripts/save_data_local.py` | FITS cube size, run duration budget, logger priorities, and logger CPU |
| `scripts/rt_on.bash` | USB IRQ number and CPU topology |

The checked-in values match one specific acquisition computer. In particular, `scripts/rt_on.bash` currently assumes a specific USB IRQ and CPU numbering. Do not run it blindly on another machine.

## Installation

### 1. Clone the repository

```bash
git clone https://github.com/LawrenceZheng5/VibrationControlSystem.git
cd VibrationControlSystem
```

### 2. Install system packages

The setup script installs the Ubuntu packages currently required by the project and by `milk`:

```bash
chmod +x setup.bash
./setup.bash
```

Also install the virtual-environment and runtime utilities if they are not already present:

```bash
sudo apt install -y python3-venv tmux
```

`setup.bash` installs packages only. It does **not** clone/build `milk`, create `/milk/shm`, configure shell environment variables, build the ALSA-only PortAudio installation, or create the Python virtual environment.

### 3. Build PortAudio with ALSA only

The low-latency acquisition system uses PortAudio without JACK or OSS support:

```bash
mkdir -p "$HOME/tools"
git clone https://github.com/PortAudio/portaudio.git "$HOME/tools/portaudio"
cd "$HOME/tools/portaudio"

./configure \
  --prefix=/usr/local \
  --without-jack \
  --without-oss

make -j"$(nproc)"
sudo make install
sudo ldconfig
```

Return to the repository afterward:

```bash
cd /path/to/VibrationControlSystem
```

### 4. Create the `milk` shared-memory directory

```bash
sudo mkdir -p /milk/shm
sudo chmod 1777 /milk/shm
```

The sticky-bit permission (`1777`) allows users to create shared-memory stream files without allowing one user to remove another user's files.

### 5. Build and install `milk`

```bash
mkdir -p "$HOME/tools"
git clone https://github.com/milk-org/milk.git "$HOME/tools/milk"
cd "$HOME/tools/milk"

mkdir -p _build
cd _build
cmake ..
make -j"$(nproc)"
sudo make install
```

The repository Makefile currently defaults to:

```text
/usr/local/milk-1.03.00
```

Set `MILK_INSTALLDIR` to the actual installation directory on your machine.

Add the following to `~/.bashrc`:

```bash
export MILK_ROOT="$HOME/tools/milk"
export MILK_INSTALLDIR="/usr/local/milk-1.03.00"
export MILK_SHM_DIR="/milk/shm"
export PATH="$MILK_INSTALLDIR/bin:$PATH"
export LD_LIBRARY_PATH="$MILK_INSTALLDIR/lib:${LD_LIBRARY_PATH:-}"
```

Reload the shell configuration:

```bash
source ~/.bashrc
```

Verify the tools and libraries:

```bash
command -v milk
command -v milk-streamFITSlog
command -v milk-nettransmit
ls -ld /milk/shm
```

### 6. Configure audio-device permissions

```bash
sudo usermod -aG audio "$USER"
```

Log out and back in before continuing, then verify:

```bash
groups
arecord -l
```

### 7. Configure real-time limits

`paRead` calls `mlockall()`, so the user must be allowed to lock memory. A typical configuration is:

```bash
sudo tee /etc/security/limits.d/test.conf >/dev/null <<'LIMITS'
@audio - rtprio 95
@audio - memlock unlimited
LIMITS
```

Log out and back in, then verify:

```bash
ulimit -r
ulimit -l
```

Expected values are a sufficiently high real-time priority limit and `unlimited` locked memory.

A PREEMPT_RT kernel is strongly recommended for acquisition testing. On supported Ubuntu releases:

```bash
sudo apt install ubuntu-realtime
sudo reboot
```

After rebooting:

```bash
uname -a
grep CONFIG_PREEMPT_RT /boot/config-"$(uname -r)"
```

### 8. Create the Python environment

```bash
cd /path/to/VibrationControlSystem
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
```
The current requirements.txt is based on the versions of pip packages on an Ubuntu 26 machine that may have different version requirements on different machines.

### 9. Build the C programs

```bash
mkdir -p bin
make
```

The Makefile builds the following executables into `bin/`:

- `paRead`
- `dataProcessEX`
- `posTrack`
- `test`
- `monitorCount`
- `monitorTime`

Verify the linked libraries:

```bash
ldd bin/paRead | grep -E 'ImageStreamIO|portaudio'
```

To rebuild:

```bash
make clean
make
```

## Hardware Verification

List the available ALSA devices:

```bash
arecord -l
```

Inspect the formats and sample rates supported by a device:

```bash
arecord -D hw:<card>,<device> --dump-hw-params
```

Confirm that the two 485B39 serial strings shown by PortAudio match `SC0` and `SC1` in `src/paRead.c`. Update the constants and rebuild if different units are installed.

## Running the System

### Recommended: acquisition and local FITS logging

Activate the Python environment and load the `milk` environment variables:

```bash
cd /path/to/VibrationControlSystem
source .venv/bin/activate
source ~/.bashrc
```

Run a two-minute smoke test:

```bash
timeout --foreground -s INT -k 60s 2m \
  ./scripts/save_data_local.py
```

Run a 12-hour overnight capture:

```bash
timeout --foreground -s INT -k 60s 12h \
  ./scripts/save_data_local.py
```

`SIGINT` is used so that `paRead` can stop cleanly and write its timing summaries. The additional 60-second kill timeout prevents a stuck process from running indefinitely.

Without an argument, the script creates the next run directory automatically:

```text
data/raw/YYYYMMDD/NN/
```
An optional path is interpreted relative to `data/raw`:

```bash
timeout --foreground -s INT -k 60s 2m \
  ./scripts/save_data_local.py 20260802/smoke_test
```

The script:

1. stops stale `paRead` and FITS logger processes;
2. creates the run directory;
3. starts `bin/paRead` and saves its console output;
4. saves the machine/run configuration;
5. waits for the `accel` stream;
6. starts `milk-streamFITSlog`;
7. waits for the first FITS file; and
8. shuts down the logger and acquisition process cleanly on `SIGINT`.

### Acquisition only

Create the output directory first:

```bash
mkdir -p data/raw/20260802/pa_read_only
```

Then run:

```bash
timeout --foreground -s INT -k 60s 2m \
  ./bin/paRead 20260802/pa_read_only \
  | tee -i data/raw/20260802/pa_read_only/console.log
```

Use `tee -i`: without it, pressing Ctrl+C may terminate `tee` before `paRead` completes its shutdown and writes all summary files.

### Monitor the shared-memory stream

```bash
milk-shmimmon accel
```

Useful process checks:

```bash
pgrep -af paRead
ps -eLo pid,tid,psr,cls,rtprio,pri,stat,comm | grep paRead
```

## Output Files

A normal local logging run creates a directory similar to:

```text
data/raw/YYYYMMDD/NN/
├── accel/
│   ├── accel_*.fits
│   └── accel_*.txt
├── console.log
├── run_config.log
├── sc0_timing.csv
├── sc0_timing_summary.csv
├── sc1_timing.csv
└── sc1_timing_summary.csv
```

The FITS files contain acceleration frames. The matching `.txt` files contain timing/index metadata written by `milk-streamFITSlog`.

### Timestamp and ordering warning

Do **not** assume that alphabetical FITS filename order is chronological. A known `milk-streamFITSlog` filename issue can produce misleading timestamps around minute boundaries. Use the timing sidecar contents through `fits_sequence_manifest.py`; the script intentionally treats the sidecar timestamp as the primary source of truth and uses FITS metadata only as a cross-check/fallback.

Also do not assume that the nominal 8 kHz acquisition rate is the effective stored rate. Check `effective_rate_hz` in the generated manifest for every run.

## Analysis Workflow

Assume the run directory is:

```bash
RUN=data/raw/20260802/01
```

### 1. Build a chronological FITS manifest

```bash
python scripts/fits_sequence_manifest.py "$RUN/accel"
```

This writes:

```text
$RUN/fits_sequence_manifest.csv
```

The manifest orders cubes chronologically, identifies continuous segments, reports missing updates between cubes, and estimates each cube's effective rate.

### 2. Analyze index continuity

```bash
python scripts/analyze_timing.py \
  "$RUN/accel" \
  --sample-rate 8000
```

By default, plots and `continuity_summary.csv` are written under:

```text
$RUN/accel/plots/
```

Additional useful options:

```bash
--print-metrics
--print-jumps
--recursive
--output-dir <directory>
```

### 3. Generate time-frequency PSD plots

```bash
python scripts/plot_psd_over_time.py \
  "$RUN/fits_sequence_manifest.csv"
```

By default, the script reads FITS files from `$RUN/accel` and writes X, Y, and Z plots to:

```text
$RUN/psd_plots/
```

Common options include:

```text
--segment
--sample-rate
--analysis-rate
--lowpass
--max-frequency
--window-seconds
--step-seconds
--vmin
--vmax
--output-dir
```

## Real-Time Tuning

`scripts/rt_on.bash` currently performs machine-specific tuning such as:

- setting the CPU governor to `performance`;
- disabling SMT;
- setting `vm.swappiness=10`;
- assigning a hard-coded USB IRQ to a hard-coded CPU; and
- disabling selected CPU idle states.

Inspect and modify the script before using it:

```bash
less scripts/rt_on.bash
```

Then, only on the intended acquisition computer:

```bash
./scripts/rt_on.bash
```

CPU isolation, callback affinity, logger affinity, and real-time priorities should be validated experimentally. A change that appears theoretically beneficial may reduce data continuity on a particular kernel or hardware topology.

## Repository Layout

```text
.
├── include/                 C headers
├── src/                     Acquisition, monitoring, and test programs
├── scripts/                 Logging, configuration, and analysis tools
├── Makefile                 C build configuration
├── requirements.txt         Python dependencies
├── setup.bash               Ubuntu package installation
└── README.md
```

Key files:

| File | Purpose |
|---|---|
| `src/paRead.c` | Dual-device PortAudio acquisition, sample matching, SHM publishing, and timing diagnostics |
| `scripts/save_data_local.py` | Local acquisition and FITS logging |
| `scripts/save_run_config.bash` | Captures system and run configuration for reproducibility |
| `scripts/fits_sequence_manifest.py` | Orders and validates FITS/timing cube sequences |
| `scripts/analyze_timing.py` | Measures Main-index continuity and missing samples |
| `scripts/plot_psd_over_time.py` | Produces X/Y/Z time-frequency PSD plots |
| `scripts/rt_on.bash` | Applies machine-specific real-time tuning |

## Troubleshooting

### `mlockall: Cannot allocate memory` or `Operation not permitted`

Check real-time limits and start a new login session:

```bash
ulimit -l
ulimit -r
```

### `libImageStreamIO.so: cannot open shared object file`

Check the installation path and library environment:

```bash
echo "$MILK_INSTALLDIR"
echo "$LD_LIBRARY_PATH"
ldd bin/paRead | grep ImageStreamIO
```

### The wrong USB devices are selected

Compare the detected device names with the `SC0` and `SC1` constants in `src/paRead.c`, update them, and rebuild.

### `milk-streamFITSlog` creates no files

Check that the stream exists, the logger attached to it, and the output path is not excessively long (Note that the whole path of the directory can't exceed 64 characters):

```bash
ls -l /milk/shm/accel.im.shm
pgrep -af 'streamFITSlog|milk-fpsCTRL'
tmux list-sessions
```

### The program does not stop cleanly

Run it through `timeout --foreground -s INT -k 60s ...` and use `tee -i` for direct `paRead` runs. Avoid piping through a process that exits immediately on Ctrl+C.

## Safety and Data Handling

- Verify accelerometer, conditioner, power, and Ethernet wiring before operating the system.
- Confirm axis labels after any sensor remounting or cable change.
- Do not commit telescope network addresses, usernames, passwords, private keys, or raw observing data.
- Preserve `run_config.log`, timing summaries, and the manifest with any dataset used for analysis.

## Acknowledgements

Developed during the 2026 Akamai Workforce Initiative internship at Subaru Telescope / SCExAO. This repository builds on the original vibration acquisition work by Jia Jun Li and on vibration-characterization research by Julien Lozi, Olivier Guyon, and collaborators.
