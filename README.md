# VibrationControlSystem
A low‑latency readout system for accelerometers using a DigiDAQ ICP USB signal conditioner on a Raspberry Pi 4, designed to feed the Subaru Adaptive Optics system.

## Overview
**VibrationControlSystem**:

- Acquires two‑channel accelerometer data at **8 kHz** via PortAudio  
- Converts raw counts to m/s² via manufacturer accelerometer calibration sheet
- Streams data to shared memory using ImageStreamIO (milk package)  

---

## Low Latency System Setup
1. **Platform**  
   Raspberry Pi 4 running the ROS 2 real‑time distro  
   <https://github.com/ros-realtime/ros-realtime-rpi4-image>

2. **Checklist**  
   - Headless real‑time kernel  
   - Disable CPU frequency scaling  
   - Shield dedicated CPU cores  
   - Use a real‑time scheduler (e.g. `SCHED_FIFO`) for the capture process
     
3. **Verify real‑time performance tool**  
   Install and run [rtcqs (Real‑Time Checker)](https://codeberg.org/rtcqs/rtcqs):

   ```bash
   sudo apt install rtcqs
   rtcqs

## Main Dependencies
- [PortAudio](https://github.com/PortAudio/portaudio) (data capture)
- [milk package](https://github.com/milk-org/milk) (high-speed shared memory)
- [ImageStreamIO](https://github.com/milk-org/ImageStreamIO) (part of milk)

## Usage

1. **Find device number and card number**
  ```bash
    arecord -l
  ```
   
2. **Find sample rate and device data**
  ```bash
  arecord -D hw:<card#>,<device#> --dump-hw-params
  ```

3. **Monitor readout**
  ```bash
  milk-shmimmon sig00'
  ```
4. **Transmit data**
  ```bash
  milk-nettransmit -T <Receiver IP Address> -s sig00 <Port Number>
  ```

