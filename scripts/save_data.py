#!/usr/bin/env python3

import signal
import subprocess
import sys
import time
from pathlib import Path

STREAM_NAMES = ["sig00"]
CUBE_SIZE = 85000
TMUX_SESSION = "milkFITSlogger"


def run_cmd(cmd, check=True, suppress_stderr=False):
    stderr = subprocess.DEVNULL if suppress_stderr else None
    return subprocess.run(cmd, check=check, stderr=stderr)


def tmux_session_exists(session_name):
    result = subprocess.run(
        ["tmux", "has-session", "-t", session_name],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return result.returncode == 0


def stop_streams(complete_cube=False):
    action = "offc" if complete_cube else "off"

    for stream in STREAM_NAMES:
        run_cmd(["milk-streamFITSlog", stream, action], check=False)
        run_cmd(["milk-streamFITSlog", stream, "kill"], check=False)


def cleanup(signum=None, frame=None):
    print("\nStopping FITS logger...")
    stop_streams(complete_cube=True)
    print("Data streaming stopped.")
    sys.exit(0)


def main():
    script_path = Path(__file__).resolve()
    project_root = script_path.parent.parent
    data_dir = project_root / "data" / "raw"

    if len(sys.argv) > 1:
        data_dir = data_dir / sys.argv[1]

    if not data_dir.exists():
        print(f"Directory {data_dir} does not exist. Creating it...")
        data_dir.mkdir(parents=True, exist_ok=True)

    print(f"Saving data to {data_dir}...")

    if tmux_session_exists(TMUX_SESSION):
        print("Stopping existing FITS logger...")
        stop_streams(complete_cube=False)
    else:
        print("No existing FITS logger running.")

    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)
    signal.signal(signal.SIGQUIT, cleanup)

    for stream in STREAM_NAMES:
        run_cmd([
            "milk-streamFITSlog",
            "-D", str(data_dir / stream),
            "-z", str(CUBE_SIZE),
            stream,
            "pstart",
        ])

    for stream in STREAM_NAMES:
        run_cmd(["milk-streamFITSlog", stream, "on"])

    print(f"Streaming {', '.join(STREAM_NAMES)} to {data_dir}. Press Ctrl+C to stop.")

    while True:
        time.sleep(1)


if __name__ == "__main__":
    main()