#!/usr/bin/env python3
"""
Create time-versus-frequency acceleration PSD plots from a
fits_sequence_manifest.csv produced for milk-streamFITSlog cubes.

Channel labels:
    SC0_CH1 -> X
    SC0_CH2 -> Y
    SC1_CH1 -> Z

Important for the current acquisition/logger format:
The FITS logger stores an entire shared-memory snapshot on every semaphore post.
The uploaded example data show that rows tagged cnt1 == 1 are not usable as
standalone SC1 snapshots. Therefore this script uses cnt1 == 0 snapshots:
    X = data[:, 0, 0]
    Y = data[:, 0, 1]
    Z = data[:, 1, 0]  (latest Z value present in that snapshot)

This is suitable for the requested low-frequency plot (default 0-30 Hz).
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from astropy.io import fits
from matplotlib.colors import LogNorm
from scipy.signal import butter, sosfilt, sosfilt_zi, spectrogram


CHANNELS = {
    "X": (0, 0),  # SC0_CH1
    "Y": (0, 1),  # SC0_CH2
    "Z": (1, 0),  # SC1_CH1
}


def read_manifest(manifest_path: Path, segment: int) -> list[dict[str, str]]:
    with manifest_path.open(newline="", encoding="utf-8") as file:
        rows = list(csv.DictReader(file))

    selected = [
        row for row in rows
        if int(row["segment"]) == segment
    ]

    if not selected:
        available = sorted({int(row["segment"]) for row in rows})
        raise ValueError(
            f"No rows found for segment {segment}. "
            f"Available segments: {available}"
        )

    selected.sort(key=lambda row: int(row["sequence"]))
    return selected


def resolve_file(data_dir: Path, value: str) -> Path:
    path = Path(value)
    return path if path.is_absolute() else data_dir / path


def load_cube_snapshots(
    fits_path: Path,
    timing_path: Path,
) -> dict[str, np.ndarray]:
    """
    Load one FITS cube and extract X, Y, and Z from cnt1 == 0 snapshots.
    """
    if not fits_path.exists():
        raise FileNotFoundError(f"Missing FITS file: {fits_path}")
    if not timing_path.exists():
        raise FileNotFoundError(f"Missing timing file: {timing_path}")

    with fits.open(fits_path, memmap=True) as hdul:
        data = np.asarray(hdul[0].data)

    timing = np.loadtxt(timing_path, comments="#", ndmin=2)

    if data.ndim != 3 or data.shape[1:] != (2, 2):
        raise ValueError(
            f"{fits_path}: expected FITS shape (frames, 2, 2), "
            f"found {data.shape}"
        )

    if timing.shape[1] < 7:
        raise ValueError(
            f"{timing_path}: expected at least 7 timing columns, "
            f"found {timing.shape[1]}"
        )

    if len(data) != len(timing):
        count = min(len(data), len(timing))
        print(
            f"WARNING: {fits_path.name} has {len(data)} FITS frames but "
            f"{len(timing)} timing rows; using the first {count}.",
            file=sys.stderr,
        )
        data = data[:count]
        timing = timing[:count]

    # Timing column 7 (zero-based index 6) is stream cnt1:
    # 0 means SC0 wrote most recently; 1 means SC1 wrote most recently.
    snapshot_mask = timing[:, 6].astype(np.int64) == 0

    if not np.any(snapshot_mask):
        raise ValueError(f"{timing_path}: contains no cnt1 == 0 snapshots")

    snapshots: dict[str, np.ndarray] = {}

    for label, (conditioner, channel) in CHANNELS.items():
        values = np.asarray(
            data[snapshot_mask, conditioner, channel],
            dtype=np.float64,
        )

        if not np.all(np.isfinite(values)):
            bad = np.count_nonzero(~np.isfinite(values))
            print(
                f"WARNING: {fits_path.name} {label} contains "
                f"{bad} non-finite values; replacing them with zero.",
                file=sys.stderr,
            )
            values = np.nan_to_num(values)

        snapshots[label] = values

    return snapshots


def stream_filter_and_downsample(
    rows: list[dict[str, str]],
    data_dir: Path,
    sample_rate: float,
    analysis_rate: float,
    lowpass_hz: float,
) -> dict[str, np.ndarray]:
    """
    Read cubes in manifest order, apply one continuous low-pass filter across
    cube boundaries, and downsample before concatenating.

    This keeps memory usage small enough for multi-hour recordings.
    """
    ratio = sample_rate / analysis_rate
    decimation = int(round(ratio))

    if decimation < 1 or not np.isclose(ratio, decimation):
        raise ValueError(
            "sample-rate / analysis-rate must be an integer. "
            f"Received {sample_rate} / {analysis_rate} = {ratio}"
        )

    if not (0.0 < lowpass_hz < analysis_rate / 2.0):
        raise ValueError(
            "lowpass-hz must be greater than zero and below the "
            "analysis-rate Nyquist frequency"
        )

    # A 10th-order Butterworth filter strongly suppresses frequencies that
    # would alias during the 8000 -> 200 Hz decimation while preserving 0-30 Hz.
    sos = butter(
        10,
        lowpass_hz,
        btype="lowpass",
        fs=sample_rate,
        output="sos",
    )

    filter_state: dict[str, np.ndarray | None] = {
        label: None for label in CHANNELS
    }
    output_chunks: dict[str, list[np.ndarray]] = {
        label: [] for label in CHANNELS
    }

    total_input_samples = 0

    for cube_number, row in enumerate(rows, start=1):
        fits_path = resolve_file(data_dir, row["fits_file"])
        timing_path = resolve_file(data_dir, row["timing_file"])

        print(
            f"[{cube_number}/{len(rows)}] "
            f"{fits_path.name}"
        )

        cube = load_cube_snapshots(fits_path, timing_path)
        cube_length = len(cube["X"])

        if any(len(cube[label]) != cube_length for label in CHANNELS):
            raise RuntimeError(
                f"{fits_path}: extracted channels have different lengths"
            )

        # Preserve the decimation phase across every FITS boundary.
        first_output_index = (-total_input_samples) % decimation

        for label in CHANNELS:
            values = cube[label]

            if filter_state[label] is None:
                # Initialize the IIR state at the first signal value to reduce
                # the startup transient.
                filter_state[label] = sosfilt_zi(sos) * values[0]

            filtered, filter_state[label] = sosfilt(
                sos,
                values,
                zi=filter_state[label],
            )

            output_chunks[label].append(
                filtered[first_output_index::decimation]
            )

        total_input_samples += cube_length

    return {
        label: np.concatenate(output_chunks[label])
        for label in CHANNELS
    }


def make_psd_plot(
    signal: np.ndarray,
    label: str,
    start_utc: str,
    output_path: Path,
    sample_rate: float,
    max_frequency: float,
    window_seconds: float,
    step_seconds: float,
    vmin: float,
    vmax: float,
    dpi: int,
) -> None:
    if len(signal) < 2:
        raise ValueError(f"Not enough {label} samples to plot")

    nperseg = int(round(window_seconds * sample_rate))
    step_samples = int(round(step_seconds * sample_rate))

    if nperseg < 2:
        raise ValueError("window-seconds is too small")
    if step_samples < 1 or step_samples > nperseg:
        raise ValueError(
            "step-seconds must be greater than zero and no larger "
            "than window-seconds"
        )
    if len(signal) < nperseg:
        raise ValueError(
            f"{label}: recording is {len(signal) / sample_rate:.3f} s long, "
            f"but the PSD window is {window_seconds:.3f} s"
        )

    noverlap = nperseg - step_samples

    # Detrending each window removes the local DC offset before calculating PSD.
    frequencies, times, psd = spectrogram(
        signal,
        fs=sample_rate,
        window="hann",
        nperseg=nperseg,
        noverlap=noverlap,
        detrend="constant",
        scaling="density",
        mode="psd",
    )

    keep = frequencies <= max_frequency
    frequencies = frequencies[keep]
    psd = psd[keep, :]

    # LogNorm cannot display zero or negative values.
    psd = np.maximum(psd, np.finfo(np.float64).tiny)

    figure, axis = plt.subplots(figsize=(9.0, 7.0))

    image = axis.pcolormesh(
        frequencies,
        times,
        psd.T,
        shading="auto",
        cmap="turbo",
        norm=LogNorm(vmin=vmin, vmax=vmax),
    )

    axis.set_xlim(0.0, max_frequency)
    axis.set_xlabel("Frequency [Hz]")
    axis.set_ylabel("Time [s]")
    axis.set_title(f"{label}\nStart: {start_utc}")

    colorbar = figure.colorbar(image, ax=axis)
    colorbar.set_label(
        r"PSD of acceleration [$(m/s^2)^2/Hz$]"
    )

    figure.tight_layout()
    figure.savefig(output_path, dpi=dpi)
    plt.close(figure)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Create X, Y, and Z time-frequency PSD plots from a "
            "FITS sequence manifest"
        )
    )
    parser.add_argument(
        "manifest",
        type=Path,
        help="Path to fits_sequence_manifest.csv",
    )
    parser.add_argument(
        "--data-dir",
        type=Path,
        default=None,
        help=(
            "Directory containing FITS/timing files. "
            "Default: manifest directory"
        ),
    )
    parser.add_argument(
        "--segment",
        type=int,
        default=1,
        help="Continuous manifest segment to plot (default: 1)",
    )
    parser.add_argument(
        "--sample-rate",
        type=float,
        default=8000.0,
        help="Physical channel sample rate in Hz (default: 8000)",
    )
    parser.add_argument(
        "--analysis-rate",
        type=float,
        default=200.0,
        help=(
            "Rate after filtered downsampling in Hz "
            "(default: 200)"
        ),
    )
    parser.add_argument(
        "--lowpass",
        type=float,
        default=50.0,
        help=(
            "Anti-alias low-pass cutoff before downsampling in Hz "
            "(default: 50)"
        ),
    )
    parser.add_argument(
        "--max-frequency",
        type=float,
        default=30.0,
        help="Maximum displayed frequency in Hz (default: 30)",
    )
    parser.add_argument(
        "--window-seconds",
        type=float,
        default=10.0,
        help=(
            "PSD window duration. A 10 s window gives 0.1 Hz "
            "frequency spacing (default: 10)"
        ),
    )
    parser.add_argument(
        "--step-seconds",
        type=float,
        default=1.0,
        help=(
            "Time between adjacent PSD rows/columns "
            "(default: 1)"
        ),
    )
    parser.add_argument(
        "--vmin",
        type=float,
        default=1.0e-10,
        help="Log color-scale minimum (default: 1e-10)",
    )
    parser.add_argument(
        "--vmax",
        type=float,
        default=1.0e-6,
        help="Log color-scale maximum (default: 1e-6)",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Output directory. Default: psd_plots beside the manifest",
    )
    parser.add_argument(
        "--dpi",
        type=int,
        default=200,
        help="PNG resolution (default: 200)",
    )
    args = parser.parse_args()

    manifest_path = args.manifest.expanduser().resolve()

    data_dir = (
        args.data_dir.expanduser().resolve()
        if args.data_dir is not None
        else manifest_path.parent / "accel"
    )

    output_dir = (
        args.output_dir.expanduser().resolve()
        if args.output_dir is not None
        else manifest_path.parent / "psd_plots"
    )

    output_dir.mkdir(parents=True, exist_ok=True)

    rows = read_manifest(manifest_path, args.segment)
    start_utc = rows[0]["start_utc"]

    print(f"Manifest: {manifest_path}")
    print(f"Data directory: {data_dir}")
    print(f"Segment: {args.segment}")
    print(f"Cubes: {len(rows)}")
    print("Channel labels: X=SC0_CH1, Y=SC0_CH2, Z=SC1_CH1")
    print("Using cnt1 == 0 synchronized/latest-value snapshots.")

    signals = stream_filter_and_downsample(
        rows=rows,
        data_dir=data_dir,
        sample_rate=args.sample_rate,
        analysis_rate=args.analysis_rate,
        lowpass_hz=args.lowpass,
    )

    for label, signal in signals.items():
        duration = len(signal) / args.analysis_rate
        output_path = (
            output_dir /
            f"segment_{args.segment}_PSD_{label}.png"
        )

        print(
            f"Plotting {label}: {len(signal)} samples, "
            f"{duration:.3f} s -> {output_path}"
        )

        make_psd_plot(
            signal=signal,
            label=label,
            start_utc=start_utc,
            output_path=output_path,
            sample_rate=args.analysis_rate,
            max_frequency=args.max_frequency,
            window_seconds=args.window_seconds,
            step_seconds=args.step_seconds,
            vmin=args.vmin,
            vmax=args.vmax,
            dpi=args.dpi,
        )

    print("Done.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
