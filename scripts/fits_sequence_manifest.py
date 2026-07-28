#!/usr/bin/env python3
"""
Build a trustworthy chronological manifest for milk-streamFITSlog cubes.

Ordering priority:
  1. Column 4 of the matching timing .txt file (absolute logging time)
  2. FITS MJD-STR, used as a cross-check and fallback

The FITS filename and filesystem modification time are intentionally ignored.
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
from astropy.io import fits


MJD_UNIX_EPOCH = 40587.0
SECONDS_PER_DAY = 86400.0


@dataclass
class Cube:
    fits_path: Path
    timing_path: Path | None
    start_unix: float
    end_unix: float
    mjd_start: float | None
    mjd_end: float | None
    first_main: int | None
    last_main: int | None
    timing_rows: int | None
    nframes: int
    segment: int = 0
    wall_gap_before_s: float | None = None
    missing_updates_before: int | None = None
    status: str = ""


def utc_string(unix_time: float) -> str:
    return datetime.fromtimestamp(unix_time, tz=timezone.utc).isoformat(
        timespec="microseconds"
    )


def mjd_to_unix(mjd: float) -> float:
    return (mjd - MJD_UNIX_EPOCH) * SECONDS_PER_DAY


def read_timing_bounds(path: Path) -> tuple[np.ndarray, np.ndarray, int]:
    """Read only the first and last data rows without loading the full file."""
    first = None
    last = None
    rows = 0

    with path.open("r", encoding="utf-8", errors="replace") as file:
        for line_number, line in enumerate(file, start=1):
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue

            values = np.fromstring(stripped, sep=" ")
            if values.size < 7:
                raise ValueError(
                    f"{path}:{line_number}: expected at least 7 columns, "
                    f"found {values.size}"
                )

            if first is None:
                first = values
            last = values
            rows += 1

    if first is None or last is None:
        raise ValueError(f"No timing rows found in {path}")

    return first, last, rows


def inspect_cube(fits_path: Path) -> Cube:
    timing_path = fits_path.with_suffix(".txt")
    if not timing_path.exists():
        timing_path = None

    with fits.open(fits_path, memmap=True) as hdul:
        header = hdul[0].header
        nframes = int(header.get("NAXIS3", 0))
        mjd_start = (
            float(header["MJD-STR"]) if "MJD-STR" in header else None
        )
        mjd_end = float(header["MJD-END"]) if "MJD-END" in header else None

    first_main = None
    last_main = None
    timing_rows = None

    if timing_path is not None:
        first, last, timing_rows = read_timing_bounds(timing_path)

        # Timing columns:
        # col2 = Main index
        # col4 = Absolute time (logging)
        start_unix = float(first[3])
        end_unix = float(last[3])
        first_main = int(first[1])
        last_main = int(last[1])
    elif mjd_start is not None and mjd_end is not None:
        start_unix = mjd_to_unix(mjd_start)
        end_unix = mjd_to_unix(mjd_end)
    else:
        raise ValueError(
            f"{fits_path}: no matching timing file and no MJD-STR/MJD-END"
        )

    return Cube(
        fits_path=fits_path,
        timing_path=timing_path,
        start_unix=start_unix,
        end_unix=end_unix,
        mjd_start=mjd_start,
        mjd_end=mjd_end,
        first_main=first_main,
        last_main=last_main,
        timing_rows=timing_rows,
        nframes=nframes,
    )


def assign_segments(cubes: list[Cube], max_gap_s: float) -> None:
    segment = 1

    for index, cube in enumerate(cubes):
        cube.segment = segment

        if index == 0:
            cube.status = "START"
            continue

        previous = cubes[index - 1]
        wall_gap = cube.start_unix - previous.end_unix
        cube.wall_gap_before_s = wall_gap

        counter_reset = (
            cube.first_main is not None
            and previous.last_main is not None
            and cube.first_main <= previous.last_main
        )
        time_reversal = cube.start_unix <= previous.start_unix
        large_gap = wall_gap > max_gap_s

        if counter_reset or time_reversal or large_gap:
            segment += 1
            cube.segment = segment

            reasons = []
            if counter_reset:
                reasons.append("COUNTER_RESET")
            if time_reversal:
                reasons.append("TIME_REVERSAL")
            if large_gap:
                reasons.append("LARGE_GAP")
            cube.status = "+".join(reasons)
            continue

        if cube.first_main is not None and previous.last_main is not None:
            cube.missing_updates_before = max(
                cube.first_main - previous.last_main - 1,
                0,
            )

        cube.status = "CONTIGUOUS"


def write_manifest(cubes: list[Cube], path: Path) -> None:
    columns = [
        "sequence",
        "segment",
        "fits_file",
        "timing_file",
        "start_utc",
        "end_utc",
        "start_unix",
        "end_unix",
        "duration_s",
        "mjd_start",
        "mjd_end",
        "first_main",
        "last_main",
        "timing_rows",
        "fits_frames",
        "wall_gap_before_s",
        "missing_updates_before",
        "status",
    ]

    with path.open("w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(file, fieldnames=columns)
        writer.writeheader()

        for sequence, cube in enumerate(cubes, start=1):
            writer.writerow(
                {
                    "sequence": sequence,
                    "segment": cube.segment,
                    "fits_file": cube.fits_path.name,
                    "timing_file": (
                        cube.timing_path.name
                        if cube.timing_path is not None
                        else ""
                    ),
                    "start_utc": utc_string(cube.start_unix),
                    "end_utc": utc_string(cube.end_unix),
                    "start_unix": f"{cube.start_unix:.9f}",
                    "end_unix": f"{cube.end_unix:.9f}",
                    "duration_s": f"{cube.end_unix - cube.start_unix:.9f}",
                    "mjd_start": (
                        f"{cube.mjd_start:.12f}"
                        if cube.mjd_start is not None
                        else ""
                    ),
                    "mjd_end": (
                        f"{cube.mjd_end:.12f}"
                        if cube.mjd_end is not None
                        else ""
                    ),
                    "first_main": (
                        cube.first_main if cube.first_main is not None else ""
                    ),
                    "last_main": (
                        cube.last_main if cube.last_main is not None else ""
                    ),
                    "timing_rows": (
                        cube.timing_rows if cube.timing_rows is not None else ""
                    ),
                    "fits_frames": cube.nframes,
                    "wall_gap_before_s": (
                        f"{cube.wall_gap_before_s:.9f}"
                        if cube.wall_gap_before_s is not None
                        else ""
                    ),
                    "missing_updates_before": (
                        cube.missing_updates_before
                        if cube.missing_updates_before is not None
                        else ""
                    ),
                    "status": cube.status,
                }
            )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Order and validate milk-streamFITSlog FITS cubes"
    )
    parser.add_argument("directory", type=Path)
    parser.add_argument(
        "--max-gap",
        type=float,
        default=1.0,
        help="Start a new segment if adjacent cubes are separated by more "
             "than this many seconds (default: 1.0)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Manifest CSV path; defaults to DIRECTORY's parent/fits_sequence_manifest.csv",
    )
    args = parser.parse_args()

    directory = args.directory.expanduser().resolve()
    fits_paths = list(directory.glob("*.fits"))
    if not fits_paths:
        raise SystemExit(f"No .fits files found in {directory}")

    cubes = [inspect_cube(path) for path in fits_paths]
    cubes.sort(key=lambda cube: cube.start_unix)
    assign_segments(cubes, args.max_gap)

    output = (
        args.output.expanduser().resolve()
        if args.output is not None
        else directory.parent / "fits_sequence_manifest.csv"
    )
    write_manifest(cubes, output)

    print(
        f"{'SEQ':>3} {'SEG':>3} {'START UTC':26} "
        f"{'FIRST':>10} {'LAST':>10} {'MISS':>6}  FILE"
    )
    for sequence, cube in enumerate(cubes, start=1):
        missing = (
            str(cube.missing_updates_before)
            if cube.missing_updates_before is not None
            else "-"
        )
        first = str(cube.first_main) if cube.first_main is not None else "-"
        last = str(cube.last_main) if cube.last_main is not None else "-"

        print(
            f"{sequence:3d} {cube.segment:3d} "
            f"{utc_string(cube.start_unix):26} "
            f"{first:>10} {last:>10} {missing:>6}  "
            f"{cube.fits_path.name}"
        )

    print(f"\nManifest written to: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
