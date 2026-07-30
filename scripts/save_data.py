#!/usr/bin/env python3

import signal
import subprocess
import sys
import time
import re
import os
import shutil
from pathlib import Path
from datetime import datetime

STREAM_NAMES = ["accel"]
CUBE_SIZE = 85000
TMUX_SESSION = "milkFITSlogger"
LOGGER_TMUX_SESSIONS = [
    TMUX_SESSION,
    *[
        f"streamFITSlog-{stream}"
        for stream in STREAM_NAMES
    ],
]

SAMPLE_RATE = 8000

# 7pm to 7am 
RUN_HOURS = 12

STREAM_UPDATE_RATE = SAMPLE_RATE


# Add a 5% margin so the logger limit is not reached before timeout/Ctrl+C.
MAX_FRAMES = int(STREAM_UPDATE_RATE * RUN_HOURS * 3600 * 1.05)

RUN_DIRECTORY_PATTERN = re.compile(
    r"^(?:run_)?(?P<number>\d+)(?:_.*)?$"
)

PA_READ_PROCESS = None
TEE_PROCESS = None

def clear_stale_fits_logger_state():
    """
    Remove persistent streamFITSlog settings left behind by a
    previous run.

    This must only be called after all previous logger processes
    and tmux sessions have stopped.
    """

    shm_root = Path("/milk/shm")
    fitslogger_root = shm_root / "FITSlogger"

    for stream in STREAM_NAMES:
        fps_name = f"streamFITSlog-{stream}"

        fps_shm = shm_root / f"{fps_name}.fps.shm"
        fps_datadir = (
            fitslogger_root
            / f"fps.{fps_name}.datadir"
        )

        if fps_shm.exists():
            fps_shm.unlink()
            print(f"Removed stale FPS state: {fps_shm}")

        if fps_datadir.exists():
            shutil.rmtree(fps_datadir)
            print(
                f"Removed stale FPS configuration: "
                f"{fps_datadir}"
            )

def logger_tmux_sessions_exist() -> bool:
    return any(
        tmux_session_exists(session_name)
        for session_name in LOGGER_TMUX_SESSIONS
    )


def kill_logger_tmux_sessions():
    for session_name in LOGGER_TMUX_SESSIONS:
        subprocess.run(
            [
                "tmux",
                "kill-session",
                "-t",
                session_name,
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )

def find_processes_exact(process_name: str) -> list[int]:
    result = subprocess.run(
        ["pgrep", "-x", process_name],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        check=False,
    )

    if result.returncode != 0:
        return []

    return [
        int(line)
        for line in result.stdout.splitlines()
        if line.strip()
    ]


def find_processes_matching(pattern: str) -> list[int]:
    result = subprocess.run(
        ["pgrep", "-f", pattern],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        check=False,
    )

    if result.returncode != 0:
        return []

    return [
        int(line)
        for line in result.stdout.splitlines()
        if line.strip()
    ]

def fits_logger_processes_exist() -> bool:
    for stream in STREAM_NAMES:
        if find_processes_matching(
            f"streamFITSlog-{stream}"
        ):
            return True

    return False


def wait_for_fits_logger_shutdown(
    timeout_seconds: float = 10.0,
):
    deadline = time.monotonic() + timeout_seconds

    while time.monotonic() < deadline:
        tmux_exists = logger_tmux_sessions_exist()
        processes_exist = fits_logger_processes_exist()

        if not tmux_exists and not processes_exist:
            print("Previous FITS logger fully stopped.")
            return

        time.sleep(0.2)

    raise RuntimeError(
        "Previous FITS logger did not fully stop. "
        "Check with:\n"
        "  pgrep -af 'streamFITSlog|milk-fpsCTRL'\n"
        f"  tmux has-session -t {TMUX_SESSION}"
    )

def stop_existing_pa_read():
    pids = find_processes_exact("paRead")

    if not pids:
        print("No existing paRead process running.")
        return

    print(
        "Stopping existing paRead process(es): "
        + ", ".join(str(pid) for pid in pids)
    )

    for pid in pids:
        try:
            os.kill(pid, signal.SIGINT)
        except ProcessLookupError:
            pass

    deadline = time.monotonic() + 30.0

    while time.monotonic() < deadline:
        remaining = find_processes_exact("paRead")

        if not remaining:
            print("Existing paRead stopped.")
            return

        time.sleep(0.2)

    remaining = find_processes_exact("paRead")

    for pid in remaining:
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass

    deadline = time.monotonic() + 5.0

    while time.monotonic() < deadline:
        remaining = find_processes_exact("paRead")

        if not remaining:
            return

        time.sleep(0.2)

    raise RuntimeError(
        "An existing paRead process would not stop: "
        + ", ".join(
            str(pid)
            for pid in find_processes_exact("paRead")
        )
    )

def start_acquisition(project_root: Path, raw_data_root: Path, data_dir: Path):
    """
    Start:

        ./bin/paRead <run-directory> |
            tee -i <absolute-run-directory>/console.log

    Then run:

        ./scripts/save_run_config.bash <run-directory>
    """

    global PA_READ_PROCESS, TEE_PROCESS

    # Example:
    # data_dir:
    #   /home/scexao/VibrationControlSystem/data/raw/20260728/run_001_190315
    #
    # run_directory_argument:
    #   20260728/run_001_190315
    run_directory_argument = data_dir.relative_to(raw_data_root).as_posix()

    pa_read_path = project_root / "bin" / "paRead"
    config_script_path = project_root / "scripts" / "save_run_config.bash"
    console_log_path = data_dir / "console.log"

    print(f"Starting acquisition for {run_directory_argument}...")

    PA_READ_PROCESS = subprocess.Popen(
        [
            str(pa_read_path),
            run_directory_argument,
        ],
        cwd=project_root,
        stdout=subprocess.PIPE,

        # Leave stderr connected to the terminal. This matches:
        #
        # ./bin/paRead ... | tee ...
        #
        # because that command does not include 2>&1.
        stderr=None,

        # Prevent Ctrl+C from directly interrupting the child.
        # The Python cleanup function will send SIGINT explicitly.
        start_new_session=True,
    )

    if PA_READ_PROCESS.stdout is None:
        raise RuntimeError("Failed to open paRead stdout pipe.")

    TEE_PROCESS = subprocess.Popen(
        [
            "tee",
            "-i",
            str(console_log_path),
        ],
        cwd=project_root,
        stdin=PA_READ_PROCESS.stdout,
    )

    # The parent does not need its duplicate of the pipe.
    PA_READ_PROCESS.stdout.close()

    # Give paRead a moment to initialize the PortAudio devices
    # and create the accel shared-memory stream.
    time.sleep(1)

    if PA_READ_PROCESS.poll() is not None:
        raise RuntimeError(
            f"paRead exited during startup with code "
            f"{PA_READ_PROCESS.returncode}"
        )

    print("Saving run configuration...")

    run_cmd(
    [
        "bash",
        str(config_script_path),
        run_directory_argument,
    ],
    check=False,
    suppress_stdout=True,
    suppress_stderr=True
    )

    print(f"Console output is being saved to {console_log_path}")

    return run_directory_argument

def create_automatic_run_directory(raw_data_root: Path) -> Path:
    """
    Create a directory such as:

        data/raw/20260728/run_001_120437
                          run_<number>_<HHMMSS>

    The date and time use the computer's configured local timezone.
    """

    start_time = datetime.now().astimezone()

    # Example: data/raw/20260728
    day_directory = raw_data_root / start_time.strftime("%Y%m%d")
    day_directory.mkdir(parents=True, exist_ok=True)

    existing_run_numbers = []

    for entry in day_directory.iterdir():
        if not entry.is_dir():
            continue

        match = RUN_DIRECTORY_PATTERN.fullmatch(entry.name)

        if match is not None:
            existing_run_numbers.append(int(match.group("number")))

    next_run_number = max(existing_run_numbers, default=0) + 1

    while True:
        # start_time_text = start_time.strftime("%H-%M-%S")

        run_directory = day_directory / (
            # File name too long with start_time embdedded in the filename
            # f"run_{next_run_number:02d}_{start_time_text}"
            f"{next_run_number:02d}"
        )

        try:
            # exist_ok=False prevents accidentally reusing a run directory.
            run_directory.mkdir(exist_ok=False)
            return run_directory

        except FileExistsError:
            # Very unlikely, but safely try the next run number.
            next_run_number += 1
            start_time = datetime.now().astimezone()

def stop_acquisition():
    global PA_READ_PROCESS, TEE_PROCESS

    if PA_READ_PROCESS is not None and PA_READ_PROCESS.poll() is None:
        print("Sending SIGINT to paRead...")

        # paRead handles SIGINT and writes its timing files before exiting.
        PA_READ_PROCESS.send_signal(signal.SIGINT)

        try:
            PA_READ_PROCESS.wait(timeout=30)
        except subprocess.TimeoutExpired:
            print("paRead did not stop after 30 seconds; sending SIGTERM...")
            PA_READ_PROCESS.terminate()

            try:
                PA_READ_PROCESS.wait(timeout=5)
            except subprocess.TimeoutExpired:
                print("paRead still did not stop; sending SIGKILL...")
                PA_READ_PROCESS.kill()
                PA_READ_PROCESS.wait()

    # tee should exit after paRead closes its stdout pipe.
    if TEE_PROCESS is not None and TEE_PROCESS.poll() is None:
        try:
            TEE_PROCESS.wait(timeout=5)
        except subprocess.TimeoutExpired:
            TEE_PROCESS.terminate()

            try:
                TEE_PROCESS.wait(timeout=2)
            except subprocess.TimeoutExpired:
                TEE_PROCESS.kill()
                TEE_PROCESS.wait()

    PA_READ_PROCESS = None
    TEE_PROCESS = None

def run_cmd(
    cmd,
    check=True,
    suppress_stdout=False,
    suppress_stderr=False,
    timeout=None,
):
    stdout = (
        subprocess.DEVNULL
        if suppress_stdout
        else None
    )
    stderr = (
        subprocess.DEVNULL
        if suppress_stderr
        else None
    )

    return subprocess.run(
        cmd,
        check=check,
        stdout=stdout,
        stderr=stderr,
        timeout=timeout,
    )

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
        try:
            run_cmd(
                [
                    "milk-streamFITSlog",
                    stream,
                    action,
                ],
                check=False,
                timeout=5,
            )
        except subprocess.TimeoutExpired:
            print(
                f"milk-streamFITSlog {stream} {action} "
                "timed out; forcing logger shutdown."
            )

    if complete_cube:
        cube_completion_time = (
            CUBE_SIZE / STREAM_UPDATE_RATE
        )
        wait_time = cube_completion_time + 2.0

        print(
            f"Waiting up to {wait_time:.1f} seconds "
            "for the current FITS cube to finish..."
        )
        time.sleep(wait_time)

    for stream in STREAM_NAMES:
        try:
            run_cmd(
                [
                    "milk-streamFITSlog",
                    stream,
                    "kill",
                ],
                check=False,
                timeout=5,
            )
        except subprocess.TimeoutExpired:
            print(
                f"milk-streamFITSlog {stream} kill "
                "timed out; killing tmux session."
            )

    # Handle both possible session naming conventions.
    kill_logger_tmux_sessions()

def cleanup(signum=None, frame=None):
    print("\nStopping FITS logger...")
    stop_streams(complete_cube=True)

    print("Stopping accelerometer acquisition...")
    stop_acquisition()

    print("Data acquisition and logging stopped.")
    sys.exit(0)

def wait_for_stream(
    stream_name: str,
    timeout_seconds: float = 10.0,
):
    if PA_READ_PROCESS is None:
        raise RuntimeError("paRead has not been started.")

    shm_path = Path("/milk/shm") / f"{stream_name}.im.shm"
    process_maps = Path(
        f"/proc/{PA_READ_PROCESS.pid}/maps"
    )

    deadline = time.monotonic() + timeout_seconds

    while time.monotonic() < deadline:
        if PA_READ_PROCESS.poll() is not None:
            raise RuntimeError(
                f"paRead exited with code "
                f"{PA_READ_PROCESS.returncode} before "
                f"{stream_name!r} became ready."
            )

        stream_exists = shm_path.exists()
        process_owns_stream = False

        if process_maps.exists():
            try:
                maps_text = process_maps.read_text(
                    encoding="utf-8",
                    errors="replace",
                )
                process_owns_stream = (
                    str(shm_path) in maps_text
                )
            except OSError:
                pass

        if stream_exists and process_owns_stream:
            print(
                f"paRead PID {PA_READ_PROCESS.pid} "
                f"has opened {shm_path}"
            )
            return

        time.sleep(0.1)

    raise TimeoutError(
        f"paRead PID {PA_READ_PROCESS.pid} did not map "
        f"{shm_path} within {timeout_seconds:.1f} seconds."
    )

def wait_for_fits_logger_attachment(
    stream_name: str,
    timeout_seconds: float = 10.0,
):
    shm_path = Path("/milk/shm") / f"{stream_name}.im.shm"
    process_pattern = f"streamFITSlog-{stream_name}"
    deadline = time.monotonic() + timeout_seconds

    while time.monotonic() < deadline:
        for pid in find_processes_matching(process_pattern):
            maps_path = Path(f"/proc/{pid}/maps")

            if not maps_path.exists():
                continue

            try:
                maps_text = maps_path.read_text(
                    encoding="utf-8",
                    errors="replace",
                )
            except OSError:
                continue

            if str(shm_path) in maps_text:
                print(
                    f"FITS logger PID {pid} attached to "
                    f"{shm_path}"
                )
                return

        time.sleep(0.1)

    raise TimeoutError(
        f"The FITS logger did not attach to {shm_path} "
        f"within {timeout_seconds:.1f} seconds."
    )

def wait_for_first_fits(
    stream_directory: Path,
    timeout_seconds: float = 30.0,
) -> bool:
    deadline = time.monotonic() + timeout_seconds

    while time.monotonic() < deadline:
        fits_files = list(stream_directory.glob("*.fits"))

        if fits_files:
            newest_file = max(
                fits_files,
                key=lambda path: path.stat().st_mtime,
            )

            print(f"First FITS file created: {newest_file}")
            return True

        if (
            PA_READ_PROCESS is not None
            and PA_READ_PROCESS.poll() is not None
        ):
            raise RuntimeError(
                f"paRead exited with code "
                f"{PA_READ_PROCESS.returncode} while "
                "waiting for a FITS file."
            )

        time.sleep(0.5)

    print(
        f"WARNING: No FITS file appeared in "
        f"{stream_directory} within "
        f"{timeout_seconds:.1f} seconds."
    )
    print("Leaving the logger running for inspection.")
    return False

def main():
    script_path = Path(__file__).resolve()
    project_root = script_path.parent.parent
    raw_data_root = project_root / "data" / "raw"

    if len(sys.argv) > 2:
        print(f"Usage: {sys.argv[0]} [optional-output-directory]")
        return 1

    if len(sys.argv) == 2:
        data_dir = raw_data_root / sys.argv[1]
        data_dir.mkdir(parents=True, exist_ok=True)
    else:
        data_dir = create_automatic_run_directory(raw_data_root)

    print(f"Saving data to {data_dir}...")

    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)
    signal.signal(signal.SIGQUIT, cleanup)

    if (
        logger_tmux_sessions_exist()
        or fits_logger_processes_exist()
    ):
        print("Stopping existing FITS logger...")
        stop_streams(complete_cube=False)
        wait_for_fits_logger_shutdown()
    else:
        print("No existing FITS logger running.")
    # Remove saved settings even when no logger process is running.
    # The FPS shared-memory file and datadir can survive after the
    # process and tmux session have stopped.
    clear_stale_fits_logger_state()

    # This must happen before starting the new paRead.
    stop_existing_pa_read()



    # This automatically runs:
    #
    # ./bin/paRead <run-dir> |
    #     tee -i <data-dir>/console.log
    #
    # ./scripts/save_run_config.bash <run-dir>
    run_directory_argument = start_acquisition(
        project_root=project_root,
        raw_data_root=raw_data_root,
        data_dir=data_dir,
    )

    for stream in STREAM_NAMES:
        wait_for_stream(stream)

    for stream in STREAM_NAMES:
        (data_dir / stream).mkdir(
            parents=True,
            exist_ok=True,
        )

    for stream in STREAM_NAMES:
        run_cmd([
            "milk-streamFITSlog",
            "-r",
            "-D", str(data_dir / stream),
            "-z", str(CUBE_SIZE),
            "-n", str(MAX_FRAMES),
            stream,
            "pstart",
        ])
    for stream in STREAM_NAMES:
        wait_for_fits_logger_attachment(stream)

    # Explicitly update the active FPS parameters after the
    # configuration and RUN processes have started.
    for stream in STREAM_NAMES:
        log_directory = data_dir / stream

        run_cmd([
            "milk-streamFITSlog",
            "-D", str(log_directory),
            "-z", str(CUBE_SIZE),
            "-n", str(MAX_FRAMES),
            stream,
            "set",
        ])

    for stream in STREAM_NAMES:
        run_cmd([
            "milk-streamFITSlog",
            stream,
            "on",
        ])

    for stream in STREAM_NAMES:
        wait_for_first_fits(data_dir / stream)


    print(
        f"Acquisition directory: {run_directory_argument}\n"
        f"Streaming {', '.join(STREAM_NAMES)} to {data_dir}.\n"
        "Press Ctrl+C to stop."
    )

    while True:
        # Stop everything if paRead unexpectedly crashes.
        if PA_READ_PROCESS is not None and PA_READ_PROCESS.poll() is not None:
            exit_code = PA_READ_PROCESS.returncode
            print(f"\npaRead unexpectedly exited with code {exit_code}.")
            cleanup()

        time.sleep(1)

if __name__ == "__main__":
    try:
        raise SystemExit(main())

    except Exception as exc:
        print(f"\nERROR: {exc}", file=sys.stderr)

        if (
            logger_tmux_sessions_exist()
            or fits_logger_processes_exist()
        ):
            print("Stopping FITS logger after error...")
            stop_streams(complete_cube=False)

        print("Stopping acquisition after error...")
        stop_acquisition()

        raise SystemExit(1)