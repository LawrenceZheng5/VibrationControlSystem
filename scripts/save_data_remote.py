#!/usr/bin/env python3

"""Save a network-received milk shared-memory stream to FITS cubes.

CONTROLLER-FIRST BUILD: milk-fpsCTRL is started and verified before pstart.

This script does not start, stop, or otherwise manage paRead or
milk-nettransmit. It waits for the received shared-memory stream, starts the
milk-fpsCTRL controller first, and then controls milk-streamFITSlog.
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import shutil
import signal
import stat
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path


STREAM_NAMES = ["accel"]
DEFAULT_CUBE_SIZE = 85_000
DEFAULT_STREAM_RATE = 8_000.0
DEFAULT_RUN_HOURS = 12.0

# This computer currently reports `ulimit -r` as 0. Keep real-time scheduling
# disabled by default; override these options after login limits are fixed.
DEFAULT_LOGGER_RTP = 0
DEFAULT_LOGGER_WRTP = 0
DEFAULT_CSET = "0"

DEFAULT_STREAM_TIMEOUT = 60.0
DEFAULT_CONTROLLER_TIMEOUT = 15.0
DEFAULT_PSTART_TIMEOUT = 30.0
DEFAULT_FIRST_FITS_TIMEOUT = 60.0
MAX_FRAME_MARGIN = 1.05

# In this milk build, the FPS dirname field preserves at most 63 bytes plus a
# terminating NUL. Longer paths are silently truncated and later produce
# CFITSIO error 105 because the truncated parent directory does not exist.
MAX_MILK_LOG_DIRECTORY_BYTES = 63

SHM_ROOT = Path(os.environ.get("MILK_SHM_DIR", "/milk/shm"))
FITSLOGGER_ROOT = SHM_ROOT / "FITSlogger"
TMUX_SESSION = "milkFITSlogger"
CONTROLLER_FIFO = SHM_ROOT / "milkFITSlogger.fifo"
ROOT_FPSTMUXENV = SHM_ROOT / "fpstmuxenv"
LOCAL_FPSTMUXENV = FITSLOGGER_ROOT / "fpstmuxenv"

LOGGER_TMUX_SESSIONS = [
    TMUX_SESSION,
    *[f"streamFITSlog-{stream}" for stream in STREAM_NAMES],
]

RUN_DIRECTORY_PATTERN = re.compile(
    r"^(?:run_)?(?P<number>\d+)(?:_.*)?$"
)

ACTIVE_CUBE_SIZE = DEFAULT_CUBE_SIZE
ACTIVE_STREAM_RATE = DEFAULT_STREAM_RATE
CLEANUP_STARTED = False


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Log the network-received accel milk stream to FITS files. "
            "This script does not start paRead or milk-nettransmit."
        )
    )
    parser.add_argument(
        "output_directory",
        nargs="?",
        help=(
            "Optional output directory. Relative paths are placed under "
            "--raw-root; absolute paths are used directly. If omitted, a "
            "YYYYMMDD/NN run directory is created automatically."
        ),
    )
    parser.add_argument(
        "--raw-root",
        type=Path,
        help=(
            "Base directory for relative and automatic run directories. "
            "Default: <project-root>/data/raw. On this installation, use a "
            "short path such as /home/aorts/vibraw."
        ),
    )
    parser.add_argument(
        "--cube-size",
        type=int,
        default=DEFAULT_CUBE_SIZE,
        help=f"Frames per FITS cube (default: {DEFAULT_CUBE_SIZE}).",
    )
    parser.add_argument(
        "--stream-rate",
        type=float,
        default=DEFAULT_STREAM_RATE,
        help=(
            "Expected stream updates per second. Used only to calculate "
            "the frame limit and graceful-stop delay "
            f"(default: {DEFAULT_STREAM_RATE:g})."
        ),
    )
    parser.add_argument(
        "--run-hours",
        type=float,
        default=DEFAULT_RUN_HOURS,
        help=(
            "Hours used to calculate milk-streamFITSlog's maximum frame "
            f"count (default: {DEFAULT_RUN_HOURS:g})."
        ),
    )
    parser.add_argument(
        "--max-frames",
        type=int,
        help=(
            "Explicit maximum frame count. Overrides --stream-rate and "
            "--run-hours for the frame limit."
        ),
    )
    parser.add_argument(
        "--rtp",
        type=int,
        default=DEFAULT_LOGGER_RTP,
        help=(
            "milk-streamFITSlog main real-time priority "
            f"(default: {DEFAULT_LOGGER_RTP})."
        ),
    )
    parser.add_argument(
        "--wrtp",
        type=int,
        default=DEFAULT_LOGGER_WRTP,
        help=(
            "milk-streamFITSlog writer real-time priority "
            f"(default: {DEFAULT_LOGGER_WRTP})."
        ),
    )
    parser.add_argument(
        "--cset",
        default=DEFAULT_CSET,
        help=(
            "Value passed to milk-streamFITSlog -cset "
            f"(default: {DEFAULT_CSET})."
        ),
    )
    parser.add_argument(
        "--cpu",
        type=int,
        help=(
            "Optional CPU on which to pin every logger thread after startup. "
            "No taskset change is made when omitted."
        ),
    )
    parser.add_argument(
        "--stream-timeout",
        type=float,
        default=DEFAULT_STREAM_TIMEOUT,
        help=(
            "Seconds to wait for /milk/shm/accel.im.shm. Use 0 to wait "
            f"forever (default: {DEFAULT_STREAM_TIMEOUT:g})."
        ),
    )
    parser.add_argument(
        "--controller-timeout",
        type=float,
        default=DEFAULT_CONTROLLER_TIMEOUT,
        help=(
            "Seconds to wait for milk-fpsCTRL to open its FIFO "
            f"(default: {DEFAULT_CONTROLLER_TIMEOUT:g})."
        ),
    )
    parser.add_argument(
        "--pstart-timeout",
        type=float,
        default=DEFAULT_PSTART_TIMEOUT,
        help=(
            "Seconds allowed for milk-streamFITSlog pstart "
            f"(default: {DEFAULT_PSTART_TIMEOUT:g})."
        ),
    )
    parser.add_argument(
        "--first-fits-timeout",
        type=float,
        default=DEFAULT_FIRST_FITS_TIMEOUT,
        help=(
            "Seconds to wait for the first FITS file "
            f"(default: {DEFAULT_FIRST_FITS_TIMEOUT:g})."
        ),
    )

    return parser.parse_args()


def run_cmd(
    cmd: list[str],
    *,
    check: bool = True,
    suppress_stdout: bool = False,
    suppress_stderr: bool = False,
    timeout: float | None = None,
) -> subprocess.CompletedProcess:
    stdout = subprocess.DEVNULL if suppress_stdout else None
    stderr = subprocess.DEVNULL if suppress_stderr else None

    return subprocess.run(
        cmd,
        check=check,
        stdout=stdout,
        stderr=stderr,
        timeout=timeout,
    )


def tmux_session_exists(session_name: str) -> bool:
    result = subprocess.run(
        ["tmux", "has-session", "-t", session_name],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.returncode == 0


def capture_tmux_pane(target: str, history: int = 150) -> str:
    result = subprocess.run(
        [
            "tmux",
            "capture-pane",
            "-p",
            "-J",
            "-t",
            target,
            "-S",
            f"-{history}",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        check=False,
    )
    return result.stdout


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


def controller_processes_exist() -> bool:
    return bool(
        find_processes_matching(
            r"milk-fpsCTRL.*milkFITSlogger\.fifo|milk -n fpsCTRL-[0-9]+"
        )
    )


def logger_tmux_sessions_exist() -> bool:
    return any(
        tmux_session_exists(session_name)
        for session_name in LOGGER_TMUX_SESSIONS
    )


def fits_logger_processes_exist() -> bool:
    return any(
        find_processes_matching(f"streamFITSlog-{stream}")
        for stream in STREAM_NAMES
    )


def kill_logger_tmux_sessions() -> None:
    for session_name in LOGGER_TMUX_SESSIONS:
        subprocess.run(
            ["tmux", "kill-session", "-t", session_name],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )


def wait_for_fits_logger_shutdown(timeout_seconds: float = 10.0) -> None:
    deadline = time.monotonic() + timeout_seconds

    while time.monotonic() < deadline:
        if (
            not logger_tmux_sessions_exist()
            and not fits_logger_processes_exist()
            and not controller_processes_exist()
        ):
            print("Previous FITS logger fully stopped.")
            return
        time.sleep(0.2)

    raise RuntimeError(
        "Previous FITS logger did not fully stop. Check with:\n"
        "  pgrep -af 'streamFITSlog|milk-fpsCTRL|fpsCTRL-'\n"
        "  tmux ls"
    )


def clear_stale_fits_logger_state() -> None:
    """Remove streamFITSlog FPS state after all logger processes stop."""

    FITSLOGGER_ROOT.mkdir(parents=True, exist_ok=True)

    for stream in STREAM_NAMES:
        fps_name = f"streamFITSlog-{stream}"
        fps_shm = SHM_ROOT / f"{fps_name}.fps.shm"
        fps_datadir = FITSLOGGER_ROOT / f"fps.{fps_name}.datadir"

        if fps_shm.exists():
            fps_shm.unlink()
            print(f"Removed stale FPS state: {fps_shm}")

        if fps_datadir.exists():
            shutil.rmtree(fps_datadir)
            print(f"Removed stale FPS configuration: {fps_datadir}")

    # The FIFO may survive after a killed controller. Only remove it after the
    # process/session shutdown checks above have confirmed there is no reader.
    if CONTROLLER_FIFO.exists():
        CONTROLLER_FIFO.unlink()
        print(f"Removed stale FITS-controller FIFO: {CONTROLLER_FIFO}")


def determine_milk_install_dir() -> Path:
    configured = os.environ.get("MILK_INSTALLDIR")
    if configured:
        return Path(configured).expanduser().resolve()

    milk_path = shutil.which("milk")
    if milk_path is None:
        raise FileNotFoundError("Could not find the 'milk' command in PATH.")

    return Path(milk_path).resolve().parent.parent


def ensure_fpstmuxenv() -> None:
    """Create the environment files expected by streamFITSlog tmux panes."""

    FITSLOGGER_ROOT.mkdir(parents=True, exist_ok=True)
    install_dir = determine_milk_install_dir()

    content = (
        f'export MILK_INSTALLDIR={shlex.quote(str(install_dir))}\n'
        f'export MILK_SHM_DIR={shlex.quote(str(SHM_ROOT))}\n'
        f'export PATH={shlex.quote(str(install_dir / "bin"))}:"$PATH"\n'
        f'export LD_LIBRARY_PATH={shlex.quote(str(install_dir / "lib"))}'
        ':"${LD_LIBRARY_PATH:-}"\n'
        'export TCSETCMDPREFIX="${TCSETCMDPREFIX:-}"\n'
        'export OMP_NUM_THREADS="${OMP_NUM_THREADS:-1}"\n'
    )

    if not ROOT_FPSTMUXENV.exists():
        ROOT_FPSTMUXENV.write_text(content, encoding="utf-8")
        print(f"Created tmux environment: {ROOT_FPSTMUXENV}")

    if not LOCAL_FPSTMUXENV.exists():
        try:
            LOCAL_FPSTMUXENV.symlink_to(Path("../fpstmuxenv"))
            print(f"Created tmux environment link: {LOCAL_FPSTMUXENV}")
        except OSError:
            # A normal file is also sufficient if symlink creation is blocked.
            LOCAL_FPSTMUXENV.write_text(content, encoding="utf-8")
            print(f"Created tmux environment: {LOCAL_FPSTMUXENV}")


def pids_with_open_path(path: Path) -> list[int]:
    """Return same-user-visible PIDs that currently have path open."""

    wanted = str(path)
    matching: list[int] = []

    try:
        proc_entries = list(Path("/proc").iterdir())
    except OSError:
        return matching

    for proc_entry in proc_entries:
        if not proc_entry.name.isdigit():
            continue

        fd_dir = proc_entry / "fd"
        try:
            fd_entries = list(fd_dir.iterdir())
        except OSError:
            continue

        for fd_entry in fd_entries:
            try:
                target = os.readlink(fd_entry)
            except OSError:
                continue

            if target == wanted:
                matching.append(int(proc_entry.name))
                break

    return matching


def wait_for_controller_ready(timeout_seconds: float) -> list[int]:
    deadline = time.monotonic() + timeout_seconds

    while time.monotonic() < deadline:
        if not tmux_session_exists(TMUX_SESSION):
            raise RuntimeError(
                "The milkFITSlogger tmux session exited before the "
                "controller became ready."
            )

        try:
            fifo_is_ready = (
                CONTROLLER_FIFO.exists()
                and stat.S_ISFIFO(CONTROLLER_FIFO.stat().st_mode)
            )
        except OSError:
            fifo_is_ready = False

        if fifo_is_ready:
            pids = pids_with_open_path(CONTROLLER_FIFO)
            if pids:
                # The manual test showed that an open FIFO is the readiness
                # condition required before pstart. Add a short settling delay.
                time.sleep(0.3)
                return pids

        time.sleep(0.1)

    pane_text = capture_tmux_pane(f"{TMUX_SESSION}:0.0")
    raise TimeoutError(
        "milk-fpsCTRL did not open its FIFO within "
        f"{timeout_seconds:.1f} seconds. Controller pane output:\n"
        f"{pane_text[-4000:]}"
    )


def start_fits_controller(timeout_seconds: float) -> None:
    """Start milk-fpsCTRL and prove the FIFO is open before pstart."""

    if tmux_session_exists(TMUX_SESSION):
        raise RuntimeError(
            f"tmux session {TMUX_SESSION!r} already exists before controller "
            "startup. Stop the previous logger first."
        )

    if CONTROLLER_FIFO.exists():
        CONTROLLER_FIFO.unlink()

    controller_command = (
        'exec env FPS_FILTSTRING_NAME="streamFITSlog" '
        f'milk-fpsCTRL -s -f {shlex.quote(str(CONTROLLER_FIFO))}'
    )

    print("STEP 1/3: starting milk-fpsCTRL controller in tmux...")
    run_cmd([
        "tmux",
        "new-session",
        "-d",
        "-s",
        TMUX_SESSION,
        controller_command,
    ])

    controller_pids = wait_for_controller_ready(timeout_seconds)
    print(
        "STEP 2/3: controller ready; FIFO is open by PID(s): "
        + ", ".join(str(pid) for pid in controller_pids)
    )


def create_automatic_run_directory(raw_data_root: Path) -> Path:
    start_time = datetime.now().astimezone()
    day_directory = raw_data_root / start_time.strftime("%Y%m%d")
    day_directory.mkdir(parents=True, exist_ok=True)

    existing_run_numbers: list[int] = []

    for entry in day_directory.iterdir():
        if not entry.is_dir():
            continue

        match = RUN_DIRECTORY_PATTERN.fullmatch(entry.name)
        if match is not None:
            existing_run_numbers.append(int(match.group("number")))

    next_run_number = max(existing_run_numbers, default=0) + 1

    while True:
        run_directory = day_directory / f"{next_run_number:02d}"

        try:
            run_directory.mkdir(exist_ok=False)
            return run_directory.resolve()
        except FileExistsError:
            next_run_number += 1


def resolve_data_directory(
    raw_data_root: Path,
    output_directory: str | None,
) -> Path:
    if output_directory is None:
        return create_automatic_run_directory(raw_data_root)

    requested = Path(output_directory).expanduser()
    data_dir = requested if requested.is_absolute() else raw_data_root / requested
    data_dir.mkdir(parents=True, exist_ok=True)
    return data_dir.resolve()


def validate_log_directory(log_directory: Path) -> Path:
    resolved = log_directory.expanduser().resolve()
    encoded_length = len(os.fsencode(str(resolved)))

    if encoded_length > MAX_MILK_LOG_DIRECTORY_BYTES:
        truncated = os.fsdecode(
            os.fsencode(str(resolved))[:MAX_MILK_LOG_DIRECTORY_BYTES]
        )
        raise ValueError(
            "MILK FITS output directory is too long.\n"
            f"  Length: {encoded_length} bytes\n"
            f"  Limit:  {MAX_MILK_LOG_DIRECTORY_BYTES} bytes\n"
            f"  Path:   {resolved}\n"
            f"  MILK would truncate it to: {truncated}\n"
            "Use a shorter --raw-root, such as /home/aorts/vibraw."
        )

    print(
        "MILK logger directory length: "
        f"{encoded_length}/{MAX_MILK_LOG_DIRECTORY_BYTES} bytes "
        f"({resolved})"
    )
    return resolved


def wait_for_stream(
    stream_name: str,
    timeout_seconds: float,
) -> None:
    """Wait for milk-nettransmit's receiver-side SHM stream to exist."""

    shm_path = SHM_ROOT / f"{stream_name}.im.shm"
    print(f"Waiting for received stream {shm_path}...")

    deadline = (
        None
        if timeout_seconds <= 0
        else time.monotonic() + timeout_seconds
    )

    while True:
        if shm_path.exists():
            print(f"Received stream is available: {shm_path}")
            return

        if deadline is not None and time.monotonic() >= deadline:
            raise TimeoutError(
                f"The received stream {shm_path} did not appear within "
                f"{timeout_seconds:.1f} seconds. Start the "
                "milk-nettransmit receiver before this logger."
            )

        time.sleep(0.2)


def wait_for_fits_logger_attachment(
    stream_name: str,
    timeout_seconds: float = 15.0,
) -> int:
    shm_path = SHM_ROOT / f"{stream_name}.im.shm"
    process_pattern = f"streamFITSlog-{stream_name}"
    deadline = time.monotonic() + timeout_seconds

    while time.monotonic() < deadline:
        for pid in find_processes_matching(process_pattern):
            maps_path = Path(f"/proc/{pid}/maps")

            try:
                maps_text = maps_path.read_text(
                    encoding="utf-8",
                    errors="replace",
                )
            except OSError:
                continue

            if str(shm_path) in maps_text:
                print(f"FITS logger PID {pid} attached to {shm_path}")
                return pid

        time.sleep(0.1)

    pane_text = capture_tmux_pane(f"streamFITSlog-{stream_name}:2.0")
    raise TimeoutError(
        f"The FITS logger did not attach to {shm_path} within "
        f"{timeout_seconds:.1f} seconds. Run-pane output:\n"
        f"{pane_text[-4000:]}"
    )


def pin_logger_to_cpu(logger_pid: int, cpu: int) -> None:
    print(f"Pinning FITS logger PID {logger_pid} and its threads to CPU {cpu}...")

    run_cmd([
        "taskset",
        "-acp",
        str(cpu),
        str(logger_pid),
    ])

    run_cmd([
        "ps",
        "-L",
        "-p",
        str(logger_pid),
        "-o",
        "pid,tid,psr,cls,rtprio,pri,ni,stat,comm",
    ])


def wait_for_first_fits(
    stream_directory: Path,
    timeout_seconds: float,
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

        if not fits_logger_processes_exist():
            pane_text = capture_tmux_pane("streamFITSlog-accel:2.0")
            raise RuntimeError(
                "The FITS logger exited while waiting for the first FITS "
                f"file. Run-pane output:\n{pane_text[-4000:]}"
            )

        time.sleep(0.5)

    print(
        f"WARNING: No FITS file appeared in {stream_directory} within "
        f"{timeout_seconds:.1f} seconds."
    )
    print("Leaving the logger running for inspection.")
    return False


def start_fits_logger(
    data_dir: Path,
    *,
    cube_size: int,
    max_frames: int,
    logger_rtp: int,
    logger_wrtp: int,
    cset: str,
    cpu: int | None,
    first_fits_timeout: float,
    pstart_timeout: float,
) -> None:
    logger_pids: dict[str, int] = {}

    for stream in STREAM_NAMES:
        log_directory = validate_log_directory(data_dir / stream)
        log_directory.mkdir(parents=True, exist_ok=True)

        pstart_command = [
            "milk-streamFITSlog",
            "-r",
            "-cset", cset,
            "-D", str(log_directory),
            "-z", str(cube_size),
            "-n", str(max_frames),
            "-rtp", str(logger_rtp),
            "-wrtp", str(logger_wrtp),
            stream,
            "pstart",
        ]

        controller_pids = pids_with_open_path(CONTROLLER_FIFO)
        if not controller_pids:
            raise RuntimeError(
                f"Controller FIFO {CONTROLLER_FIFO} is no longer open; "
                "refusing to call pstart."
            )

        print(
            "STEP 3/3: calling pstart only after milk-fpsCTRL opened "
            f"{CONTROLLER_FIFO} (PID(s): "
            + ", ".join(str(pid) for pid in controller_pids)
            + ")..."
        )

        try:
            run_cmd(pstart_command, timeout=pstart_timeout)
        except subprocess.TimeoutExpired as exc:
            controller_text = capture_tmux_pane(f"{TMUX_SESSION}:0.0")
            raise TimeoutError(
                "milk-streamFITSlog pstart timed out despite the controller "
                f"readiness check. Controller output:\n{controller_text[-4000:]}"
            ) from exc

        logger_pids[stream] = wait_for_fits_logger_attachment(stream)

        run_cmd([
            "milk-streamFITSlog",
            "-cset", cset,
            "-D", str(log_directory),
            "-z", str(cube_size),
            "-n", str(max_frames),
            "-rtp", str(logger_rtp),
            "-wrtp", str(logger_wrtp),
            stream,
            "set",
        ])

        run_cmd([
            "milk-streamFITSlog",
            stream,
            "on",
        ])

        if cpu is not None:
            pin_logger_to_cpu(logger_pids[stream], cpu)

    for stream in STREAM_NAMES:
        wait_for_first_fits(
            validate_log_directory(data_dir / stream),
            timeout_seconds=first_fits_timeout,
        )


def stop_streams(*, complete_cube: bool) -> None:
    action = "offc" if complete_cube else "off"

    for stream in STREAM_NAMES:
        try:
            run_cmd(
                ["milk-streamFITSlog", stream, action],
                check=False,
                timeout=5,
            )
        except subprocess.TimeoutExpired:
            print(
                f"milk-streamFITSlog {stream} {action} timed out; "
                "forcing logger shutdown."
            )

    if complete_cube and fits_logger_processes_exist():
        nominal_cube_time = ACTIVE_CUBE_SIZE / ACTIVE_STREAM_RATE
        wait_time = max(nominal_cube_time * 1.5 + 2.0, 5.0)

        print(
            f"Waiting up to {wait_time:.1f} seconds for the current "
            "FITS cube to finish..."
        )
        time.sleep(wait_time)

    for stream in STREAM_NAMES:
        try:
            run_cmd(
                ["milk-streamFITSlog", stream, "kill"],
                check=False,
                timeout=5,
            )
        except subprocess.TimeoutExpired:
            print(
                f"milk-streamFITSlog {stream} kill timed out; "
                "killing the logger tmux session."
            )

    kill_logger_tmux_sessions()

    # Remove the FIFO only after its tmux session has been killed.
    try:
        if CONTROLLER_FIFO.exists():
            CONTROLLER_FIFO.unlink()
    except OSError:
        pass


def cleanup(signum: int | None = None, frame=None) -> None:
    del frame
    global CLEANUP_STARTED

    if CLEANUP_STARTED:
        return

    CLEANUP_STARTED = True

    if signum is not None:
        print(f"\nReceived signal {signum}.")

    print("Stopping remote FITS logger...")
    stop_streams(complete_cube=True)
    print("Remote FITS logging stopped.")

    if signum is not None:
        raise SystemExit(0)


def validate_args(args: argparse.Namespace) -> None:
    if args.cube_size <= 0:
        raise ValueError("--cube-size must be greater than zero.")
    if args.stream_rate <= 0:
        raise ValueError("--stream-rate must be greater than zero.")
    if args.run_hours <= 0:
        raise ValueError("--run-hours must be greater than zero.")
    if args.max_frames is not None and args.max_frames <= 0:
        raise ValueError("--max-frames must be greater than zero.")
    if args.cpu is not None and args.cpu < 0:
        raise ValueError("--cpu cannot be negative.")
    if args.controller_timeout <= 0:
        raise ValueError("--controller-timeout must be greater than zero.")
    if args.pstart_timeout <= 0:
        raise ValueError("--pstart-timeout must be greater than zero.")


def main() -> int:
    global ACTIVE_CUBE_SIZE, ACTIVE_STREAM_RATE

    args = parse_args()
    validate_args(args)


    ACTIVE_CUBE_SIZE = args.cube_size
    ACTIVE_STREAM_RATE = args.stream_rate

    script_path = Path(__file__).resolve()
    project_root = script_path.parent.parent
    raw_data_root = (
        args.raw_root.expanduser().resolve()
        if args.raw_root is not None
        else (project_root / "data" ).resolve()
    )

    data_dir = resolve_data_directory(raw_data_root, args.output_directory)

    # Validate every final -D path before starting or stopping any MILK process.
    for stream in STREAM_NAMES:
        validate_log_directory(data_dir / stream)

    max_frames = (
        args.max_frames
        if args.max_frames is not None
        else int(
            args.stream_rate
            * args.run_hours
            * 3600
            * MAX_FRAME_MARGIN
        )
    )

    print(f"Saving remote accel data to {data_dir}...")
    print(f"FITS cube size: {args.cube_size} frames")
    print(f"Maximum frames: {max_frames}")
    print(
        "FITS scheduling: "
        f"rtp={args.rtp}, wrtp={args.wrtp}, cset={args.cset}"
    )

    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)
    signal.signal(signal.SIGQUIT, cleanup)

    if (
        logger_tmux_sessions_exist()
        or fits_logger_processes_exist()
        or controller_processes_exist()
    ):
        print("Stopping existing accel FITS logger/controller...")
        stop_streams(complete_cube=False)
        wait_for_fits_logger_shutdown()
    else:
        print("No existing accel FITS logger/controller running.")

    clear_stale_fits_logger_state()

    for stream in STREAM_NAMES:
        wait_for_stream(stream, timeout_seconds=args.stream_timeout)

    # This ordering is essential on aorts25. Calling pstart first can block on
    # `echo confstart ... >> milkFITSlogger.fifo` before fpsCTRL is a reader.
    ensure_fpstmuxenv()
    start_fits_controller(timeout_seconds=args.controller_timeout)

    start_fits_logger(
        data_dir,
        cube_size=args.cube_size,
        max_frames=max_frames,
        logger_rtp=args.rtp,
        logger_wrtp=args.wrtp,
        cset=args.cset,
        cpu=args.cpu,
        first_fits_timeout=args.first_fits_timeout,
        pstart_timeout=args.pstart_timeout,
    )

    print(
        f"Logging received stream(s) {', '.join(STREAM_NAMES)} to {data_dir}.\n"
        "paRead and milk-nettransmit are not managed by this script.\n"
        "Press Ctrl+C to stop the FITS logger."
    )

    while True:
        if not fits_logger_processes_exist():
            raise RuntimeError("The accel FITS logger unexpectedly exited.")
        time.sleep(1)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        cleanup()
        raise SystemExit(0)
    except Exception as exc:
        print(f"\nERROR: {exc}", file=sys.stderr)

        if (
            logger_tmux_sessions_exist()
            or fits_logger_processes_exist()
            or controller_processes_exist()
        ):
            print("Stopping FITS logger/controller after error...")
            stop_streams(complete_cube=False)

        raise SystemExit(1)