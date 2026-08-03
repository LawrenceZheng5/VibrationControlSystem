# Vibration Control System User Guide

## 1. Overview

The Vibration Control System records accelerometer measurements from the Subaru Telescope vibration-monitoring hardware.

Accelerometer measurements are acquired by the DAQ computer and published to a MILK shared-memory stream named:

```text
accel
```

The data can then be recorded in one of two ways:

1. **Local logging**  
   Record the `accel` stream directly on the DAQ computer using `save_data_local.py`.

2. **Remote logging**  
   Transmit the `accel` stream over the network using `milk-nettransmit` and record it on the `aorts25` computer using `save_data_remote.py`.

The current nominal acquisition rate is:

```text
8,000 complete accelerometer frames per second
```

Each complete frame contains the synchronized X, Y, and Z accelerometer measurements.

---

# 2. System Architecture

## Local logging

```text
Accelerometers
      |
      v
USB signal conditioners
      |
      v
paRead
      |
      v
MILK shared-memory stream: accel
      |
      v
milk-streamFITSlog
      |
      v
FITS files stored on the DAQ computer
```

## Remote logging

```text
Accelerometers
      |
      v
USB signal conditioners
      |
      v
paRead on DAQ computer
      |
      v
MILK shared-memory stream: accel
      |
      v
milk-nettransmit
      |
      v
Network connection
      |
      v
MILK shared-memory stream on aorts25
      |
      v
milk-streamFITSlog
      |
      v
FITS files stored on aorts25
```

---

# 3. Automatic Startup and Data Collection

The DAQ computer uses `systemd` services to:

- Apply the real-time configuration automatically during boot.
- Start local data collection every day at 7:00 PM.
- Stop local data collection at approximately 7:00 AM after a 12-hour run.

The relevant files are:

```text
/etc/systemd/system/vibration-capture.service
/etc/systemd/system/vibration-capture.timer
/etc/systemd/system/vibration-rt.service
```

---

## 3.1 Real-Time Configuration Service

The following service applies the DAQ computer's real-time settings during boot:

```text
/etc/systemd/system/vibration-rt.service
```

The service runs the real-time configuration script used by the vibration acquisition system.

The real-time settings may include:

- CPU performance governor configuration.
- Real-time scheduling limits.
- Memory-locking configuration.
- CPU or interrupt affinity.
- Other operating-system settings intended to reduce acquisition latency.

Check the service status with:

```bash
sudo systemctl status vibration-rt.service
```

View the most recent service logs with:

```bash
journalctl -u vibration-rt.service
```

Restart the service manually with:

```bash
sudo systemctl restart vibration-rt.service
```

The real-time service should normally complete during boot before an acquisition begins.

---

## 3.2 Automatic Capture Service

The local data-acquisition service is:

```text
/etc/systemd/system/vibration-capture.service
```

This service starts the local data-acquisition script and allows it to run for approximately 12 hours.

The service performs the equivalent of starting:

```bash
./scripts/save_data_local.py
```

The capture service should not normally be started manually when the nightly timer is enabled. It can, however, be started manually for testing:

```bash
sudo systemctl start vibration-capture.service
```

Check its status with:

```bash
sudo systemctl status vibration-capture.service
```

Stop it manually with:

```bash
sudo systemctl stop vibration-capture.service
```

View the service logs with:

```bash
journalctl -u vibration-capture.service
```

Follow the logs in real time with:

```bash
journalctl -fu vibration-capture.service
```

---

## 3.3 Automatic Capture Timer

The timer that schedules nightly data collection is:

```text
/etc/systemd/system/vibration-capture.timer
```

The timer is configured to start the capture service at approximately:

```text
7:00 PM HST
```

The acquisition runs for approximately 12 hours and ends at approximately:

```text
7:00 AM HST
```

Check the timer with:

```bash
systemctl list-timers vibration-capture.timer
```

Check the timer status with:

```bash
sudo systemctl status vibration-capture.timer
```

Enable the timer at boot with:

```bash
sudo systemctl enable vibration-capture.timer
```

Start the timer immediately with:

```bash
sudo systemctl start vibration-capture.timer
```

Enable and start it in one command with:

```bash
sudo systemctl enable --now vibration-capture.timer
```

Disable the timer with:

```bash
sudo systemctl disable --now vibration-capture.timer
```

After modifying any `systemd` file, reload the configuration:

```bash
sudo systemctl daemon-reload
```

---

# 4. Local Data Collection

## 4.1 Purpose

The local acquisition script is:

```text
scripts/save_data_local.py
```

This script records accelerometer data directly on the DAQ computer.

The DAQ computer receives its network address using DHCP. Its IP address may therefore change after a reboot or after reconnecting it to the network. It should be `133.40.160.221`

Check the current DAQ IP address with:

```bash
hostname -I
```

Additional network information can be viewed with:

```bash
ip addr
```

or:

```bash
ip route
```

Do not assume that the DAQ computer will always have the same IP address unless a DHCP reservation or static address has been configured.

---

## 4.2 Starting a Local Acquisition

From the repository directory, activate the Python environment if necessary:

```bash
source .venv/bin/activate
```

Start a local acquisition with:

```bash
./scripts/save_data_local.py
```

For a fixed-duration test, use `timeout`. For example, a 10-minute test can be started with:

```bash
timeout --foreground -s INT -k 60s 10m ./scripts/save_data_local.py
```

For a 12-hour run:

```bash
timeout --foreground -s INT -k 60s 12h ./scripts/save_data_local.py
```

The `--foreground` and `-s INT` options allow the script to perform its normal shutdown procedure when the timeout expires.

---

## 4.3 What `save_data_local.py` Does

At a high level, `save_data_local.py`:

1. Creates a new run directory.
2. Checks for existing `paRead` or FITS logger processes.
3. Starts `paRead`.
4. Waits for the `accel` shared-memory stream to appear.
5. Starts `milk-streamFITSlog`.
6. Waits for the FITS logger to attach to the stream.
7. Waits for the first FITS file to be created.
8. Records console output and run configuration information.
9. Continues logging until the program is stopped or the timeout expires.
10. Stops the FITS logger.
11. Allows the current FITS cube to finish when possible.
12. Stops `paRead`.
13. Cleans up remaining logger processes and `tmux` sessions.

---

## 4.4 Stopping a Local Acquisition

For an acquisition started in a terminal, press:

```text
Ctrl+C
```

The script should then:

- Stop FITS logging.
- Complete the current FITS cube when possible.
- Stop accelerometer acquisition.
- Save the timing summaries.
- Clean up the associated processes.

Avoid immediately using `kill -9`, because it prevents the program from performing a normal shutdown and may leave:

- Incomplete FITS files.
- Missing timing summaries.
- Stale shared-memory streams.
- Remaining `tmux` sessions.
- Background `paRead` or FITS logger processes.

When running through `systemd`, stop the acquisition with:

```bash
sudo systemctl stop vibration-capture.service
```

---

## 4.5 Local Output Files

A typical run directory contains files similar to:

```text
data/raw/YYYYMMDD/NN/
├── accel/
│   ├── accel_*.fits
│   └── accel_*.txt
├── console.log
├── run_config.log
├── fits_sequence_manifest.csv
├── sc0_timing.csv
├── sc0_timing_summary.csv
├── sc1_timing.csv
├── sc1_timing_summary.csv
└── psd_plots/
```

### `accel/*.fits`

Contains accelerometer data recorded as FITS cubes.

### `accel/*.txt`

Contains timing information associated with the FITS cubes.

The timing-file contents should be used as the source of truth for ordering the data because the timestamp in a FITS filename may not always be reliable.

### `console.log`

Contains the terminal output produced during acquisition.

Use this file to identify:

- PortAudio errors.
- Stream startup failures.
- FITS logger failures.
- Acquisition timing statistics.
- Shutdown problems.

### `run_config.log`

Contains the software and computer configuration associated with the run.

This file should be kept with the dataset so the acquisition can be reproduced and interpreted later.

### `sc0_timing.csv` and `sc1_timing.csv`

Contain detailed timing events reported by the two USB signal conditioners.

### Timing summary files

The files:

```text
sc0_timing_summary.csv
sc1_timing_summary.csv
```

contain summarized timing statistics for each signal conditioner.

### `fits_sequence_manifest.csv`

Contains the corrected chronological ordering and timing information for the FITS and timing files.

### `psd_plots/`

Contains Power Spectral Density plots generated from the recorded accelerometer data.

---

# 5. Local Logging Limitations

The current local logging configuration has the following known limitation:

> `milk-streamFITSlog` does not always consume and save the full 8 kHz `accel` stream fast enough.

Although `paRead` publishes complete frames at a nominal rate of 8 kHz, the FITS logger may save data at a lower effective rate.

Observed logging rates have sometimes been approximately:

```text
5 kHz to 6.5 kHz
```

depending on the computer configuration and logger settings.

This does not necessarily mean that `paRead` is acquiring at that lower rate. The limitation may occur between:

```text
MILK shared-memory publication
```

and:

```text
milk-streamFITSlog writing the data to disk
```

The effective saved rate should be checked for every important dataset using the generated timing files and manifest.

---

## 5.1 Local Logger Optimization Work

The following changes should be tested to improve local FITS logging performance:

- Pin the FITS logger to a specific CPU.
- Isolate the selected logger CPU from general operating-system tasks.
- Confirm that the logger is actually using a real-time scheduling class.
- Test different logger real-time priorities.
- Test different FITS writer real-time priorities.
- Separate the acquisition and FITS-writing workloads onto different CPU cores.
- Confirm that USB interrupts are not running on the same CPU as the FITS writer.
- Test different FITS cube sizes.
- Check whether storage write speed is limiting performance.
- Reduce unnecessary background processes.
- Compare real-time and non-real-time kernels.
- Confirm that the CPU governor remains in performance mode.
- Check whether the logger thread and writer thread require separate CPU affinity settings.

Relevant `milk-streamFITSlog` options include:

```text
-rtp
-wrtp
-cset
```

However, setting these options does not guarantee that the process is actually running with real-time scheduling. Verify the process after startup.

Find the logger process with:

```bash
pgrep -af streamFITSlog
```

Inspect its scheduling class, priority, and CPU with:

```bash
ps -T -p <LOGGER_PID> \
    -o pid,tid,psr,cls,rtprio,pri,pcpu,stat,comm,wchan:24
```

Check CPU affinity with:

```bash
taskset -acp <LOGGER_PID>
```

A real-time process should normally show a real-time scheduling class such as `FF` or `RR`, rather than:

```text
TS
```

---

# 6. Remote Data Collection

## 6.1 Purpose

The remote acquisition script is:

```text
scripts/save_data_remote.py
```

The goal of remote logging is to:

1. Acquire accelerometer data on the DAQ computer.
2. Transmit the `accel` shared-memory stream over the network.
3. Recreate the stream on `aorts25`.
4. Save the FITS files on `aorts25`.

The remote computer is:

```text
Host: aorts25
IP address: 133.40.163.189
```

---

## 6.2 Start the Receiver on `aorts25`

Before starting transmission from the DAQ computer, start the receiver on `aorts25`.

On `aorts25`, run:

```bash
milk-nettransmit 30100
```

This listens for an incoming stream on TCP port:

```text
30100
```

Check whether the port is already in use with:

```bash
netstat -lntu | grep ":30100"
```

No output normally means that the port is available.

The receiver is often run inside a persistent `tmux` session:

```bash
tmux new -s accel-receiver
```

Then run:

```bash
milk-nettransmit 30100
```

Detach from the session with:

```text
Ctrl+B, then D
```

Reconnect later with:

```bash
tmux attach -t accel-receiver
```

---

## 6.3 Start Transmission on the DAQ Computer

After the receiver is running on `aorts25`, start transmission from the DAQ computer:

```bash
milk-nettransmit -s accel 30100 -T 133.40.163.189
```

This command:

- Selects the `accel` stream.
- Uses port `30100`.
- Sends the stream to `133.40.163.189`.
- Uses TCP unless UDP is explicitly requested.

The receiving process on `aorts25` creates or updates a corresponding MILK shared-memory stream.

Verify that the stream exists on `aorts25` before starting the remote FITS logger.

---

## 6.4 Start Remote FITS Logging

Once the `accel` stream is present on `aorts25`, start:

```bash
./scripts/save_data_remote.py
```

The remote script is intended to:

1. Wait for `/milk/shm/accel.im.shm`.
2. Create a new run directory.
3. Start `milk-streamFITSlog`.
4. Configure the logger.
5. Start FITS recording.
6. Stop the logger cleanly when interrupted.

The script is currently a work in progress and should be monitored while it is running.

---

## 6.5 Remote Streaming Performance

Testing with `milk-nettransmit` has transferred approximately:

```text
7.8 kHz out of the nominal 8 kHz stream
```

to `aorts25`.

This is close to the desired rate, but it still represents some lost or delayed updates.

The remote system has two separate potential bottlenecks:

1. Transmitting the shared-memory stream from the DAQ computer to `aorts25`.
2. Saving the received stream to FITS files on `aorts25`.

A good network receive rate does not guarantee that the remote FITS logger is saving at the same rate.

The effective rate must therefore be checked using the FITS timing files after every important remote test.

---

# 7. Remote Logging Limitations

`save_data_remote.py` is currently a work in progress.

Known or possible limitations include:

- The script may wait indefinitely if the `accel` stream does not appear.
- The `milk-nettransmit` receiver must be started before transmission.
- The remote logger may not save every received stream update.
- FITS logger CPU affinity and real-time priority may not be applied correctly.
- The remote machine may use a different MILK installation path.
- Old FITS logger processes or `tmux` sessions may interfere with a new run.
- A network interruption can stop or delay the stream.
- The remote script may require manual cleanup after an unsuccessful test.
- The effective saved sample rate may be lower than the transmitted rate.
- The shutdown sequence is still being tested.

Remote logging should not yet be treated as fully unattended production operation.

---

# 8. Verifying an Acquisition

During either local or remote acquisition, verify the following:

## Check `paRead`

```bash
pgrep -af paRead
```

## Check the shared-memory stream

```bash
ls -lh /milk/shm/accel.im.shm
```

## Check the FITS logger

```bash
pgrep -af streamFITSlog
```

## Check `tmux` sessions

```bash
tmux ls
```

## Check that FITS files are being created

```bash
find <run_directory>/accel -maxdepth 1 -name '*.fits' | tail
```

## Watch the number of FITS files increase

```bash
watch -n 2 "find <run_directory>/accel -maxdepth 1 -name '*.fits' | wc -l"
```

## Check available storage

```bash
df -h
```

Long acquisitions create large amounts of data. Always verify available storage before starting an overnight run.

---

# 9. Troubleshooting

## 9.1 The `accel` Stream Does Not Appear

Check whether `paRead` is running:

```bash
pgrep -af paRead
```

Check whether both USB signal conditioners are detected:

```bash
arecord -l
```

Review the acquisition output:

```bash
tail -n 100 <run_directory>/console.log
```

Possible causes include:

- A disconnected USB signal conditioner.
- A changed ALSA device number.
- A conditioner serial number that does not match the source-code configuration.
- Another process already using the audio device.
- `paRead` failing during initialization.

---

## 9.2 FITS Files Are Not Appearing

Check whether the logger is running:

```bash
pgrep -af streamFITSlog
```

Check the logger `tmux` sessions:

```bash
tmux ls
```

Check whether the logger has opened the shared-memory stream:

```bash
grep -l '/milk/shm/accel.im.shm' /proc/*/maps 2>/dev/null
```

Possible causes include:

- The logger started before the stream existed.
- The logger is attached to the wrong stream.
- The output directory is invalid.
- The output path is too long.
- A stale FPS configuration is being reused.
- A previous logger process is still running.
- The logger has reached a configured frame or cube limit.

---

## 9.3 Remote Stream Does Not Appear

On `aorts25`, confirm that the receiver is running:

```bash
pgrep -af milk-nettransmit
```

Confirm that port `30100` is listening:

```bash
netstat -lntu | grep ":30100"
```

From the DAQ computer, test network access:

```bash
ping 133.40.163.189
```

Confirm the sender is running:

```bash
pgrep -af milk-nettransmit
```

Confirm that the correct command was used:

```bash
milk-nettransmit -s accel 30100 -T 133.40.163.189
```

---

## 9.4 Acquisition Does Not Stop Cleanly

Check for remaining processes:

```bash
pgrep -af paRead
pgrep -af streamFITSlog
pgrep -af milk-nettransmit
```

Check `tmux`:

```bash
tmux ls
```

Use the normal FITS logger shutdown command before killing the process:

```bash
milk-streamFITSlog accel offc
```

Then stop the logger:

```bash
milk-streamFITSlog accel kill
```

Only use forced process termination after the normal shutdown procedure fails.

---

# 10. Known Limitations

The current system has the following known limitations:

- The nominal acquisition rate is 8 kHz, but the effective FITS logging rate may be lower.
- Local `milk-streamFITSlog` performance is not yet sufficient to guarantee that every published frame is saved.
- Remote `milk-nettransmit` testing has achieved approximately 7.8 kHz out of 8 kHz.
- The remote logger may introduce additional frame loss.
- FITS filename timestamps may not be strictly chronological.
- Timing sidecar files and the generated manifest should be used for chronological ordering.
- Real-time priorities requested through command-line options may not actually be applied.
- CPU affinity and interrupt affinity are machine-specific.
- The DAQ IP address is assigned by DHCP and may change.
- `save_data_remote.py` is still under development.
- Remote logging has not yet been fully validated for unattended overnight operation.

---

# 11. TODO

## Local Logging

- Optimize `milk-streamFITSlog` so it can save the complete 8 kHz stream.
- Test additional real-time scheduling settings.
- Verify that logger and writer real-time priorities are applied.
- Pin the logger to a dedicated CPU.
- Isolate the logger CPU from general-purpose processes.
- Test separating the logger and writer threads onto different CPUs.
- Measure whether storage write speed is a bottleneck.
- Compare different FITS cube sizes.
- Compare local logging performance across multiple real-time configurations.
- Record the effective saved sample rate automatically in the run summary.

## Remote Logging

- Complete and validate `save_data_remote.py`.
- Improve its startup checks and error handling.
- Add a timeout when waiting for the remote `accel` stream.
- Automatically verify that `milk-nettransmit` is receiving updates.
- Optimize FITS logging on `aorts25`.
- Pin the remote FITS logger to a dedicated CPU.
- Verify real-time priority on the remote logger and writer.
- Compare remote logging performance against local logging.
- Add automatic cleanup of stale logger processes and `tmux` sessions.
- Detect network interruptions.
- Record transmitted, received, and saved update rates separately.
- Test unattended remote logging for a complete 12-hour observing period.

---

# 12. Recommended Operating Procedure

For the current version of the system:

1. Confirm that both USB signal conditioners are connected.
2. Confirm that the DAQ computer is reachable on the network.
3. Check available disk space.
4. Confirm that `vibration-rt.service` completed successfully.
5. Confirm that the `accel` stream appears after starting acquisition.
6. Confirm that the FITS logger attaches to the correct stream.
7. Confirm that the first FITS file is created.
8. Monitor the acquisition during the first several minutes.
9. After the run, generate or inspect the FITS sequence manifest.
10. Check the effective saved update rate.
11. Review the timing summaries and console log.
12. Keep `run_config.log` with the recorded dataset.

Until the logger performance issues are resolved, every dataset should be checked for continuity before it is used for scientific analysis.