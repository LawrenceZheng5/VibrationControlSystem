#!/usr/bin/env python3

"""
Analyze milk-streamFITSlog FITS cubes from sig00.

Expected acquisition layout from paRead:

    size[0] = CHANNELS           = 2
    size[1] = NUM_SC             = 2
    size[2] = FRAMES_PER_BUFFER  = 1

Each frame contains:

    SC0_CH1 = conditioner 0, channel 0
    SC0_CH2 = conditioner 0, channel 1
    SC1_CH1 = conditioner 1, channel 0
    SC1_CH2 = conditioner 1, channel 1

The C indexing is:

    index = ch + CHANNELS * conditionerIndex
              + CHANNELS * NUM_SC * frame

So the flattened order per frame is:

    [SC0_CH1, SC0_CH2, SC1_CH1, SC1_CH2]

Units should be m/s^2 if paRead already applied the calibration scale.
"""

from pathlib import Path
import argparse

import numpy as np
import matplotlib.pyplot as plt
from matplotlib.colors import LogNorm
from astropy.io import fits
from scipy.signal import welch, spectrogram, find_peaks


SAMPLE_RATE = 8000.0  # Hz

CHANNEL_NAMES = [
    "SC0_CH1",
    "SC0_CH2",
    "SC1_CH1",
    "SC1_CH2_UNUSED",
]


def read_fits_data(path: Path) -> np.ndarray:
    """
    Read data from a FITS file.

    milk-streamFITSlog usually stores the data in the primary HDU,
    but this checks extensions too just in case.
    """
    with fits.open(path) as hdul:
        print("\nFITS file structure:")
        hdul.info()

        data = None
        for hdu in hdul:
            if hdu.data is not None:
                data = hdu.data
                break

    if data is None:
        raise ValueError(f"No image data found in {path}")

    data = np.asarray(data)
    print(f"\nRaw FITS data shape: {data.shape}")
    print(f"Raw FITS data dtype:  {data.dtype}")

    return data


def convert_to_time_by_channel(data: np.ndarray) -> np.ndarray:
    """
    Convert the FITS cube into a simple array:

        output shape = (num_samples, 4)

    Columns are:

        0: SC0_CH1
        1: SC0_CH2
        2: SC1_CH1
        3: SC1_CH2_UNUSED

    Because FITS/milk dimension order can appear differently depending
    on how the cube was written, this function tries to infer the layout.
    """
    data = np.squeeze(data)
    print(f"Squeezed data shape: {data.shape}")

    # Case 1:
    # Already flattened as (num_frames, 4)
    if data.ndim == 2 and data.shape[-1] == 4:
        out = data.astype(float)

    # Case 2:
    # Already flattened as (4, num_frames)
    elif data.ndim == 2 and data.shape[0] == 4:
        out = data.T.astype(float)

    # Case 3:
    # Shape contains dimensions 2 and 2 plus one large time dimension.
    # Example possibilities:
    #   (num_frames, 2, 2)
    #   (2, 2, num_frames)
    #   (2, num_frames, 2)
    elif data.ndim == 3:
        shape = data.shape
        time_axis = int(np.argmax(shape))

        # Move time axis to the front.
        x = np.moveaxis(data, time_axis, 0)

        print(f"After moving time axis first: {x.shape}")

        # Now x should be roughly (num_frames, 2, 2).
        if x.shape[1:] != (2, 2):
            raise ValueError(
                "Could not interpret 3D data layout. "
                f"After moving time axis first, got {x.shape}, expected (N, 2, 2)."
            )

        # Need columns in the same order as the C flattened stream:
        # [ch0 cond0, ch1 cond0, ch0 cond1, ch1 cond1]
        #
        # If x is indexed as x[time, ch, conditioner], this is:
        out = np.column_stack([
            x[:, 0, 0],
            x[:, 1, 0],
            x[:, 0, 1],
            x[:, 1, 1],
        ]).astype(float)

    # Case 4:
    # Shape contains an extra singleton dimension, after squeeze failed to simplify enough.
    elif data.ndim > 3:
        flat = data.reshape(-1, 4)
        out = flat.astype(float)

    else:
        raise ValueError(f"Unsupported data shape: {data.shape}")

    print(f"Converted data shape: {out.shape}  # (samples, 4 channels)")

    return out


def summarize_channels(x: np.ndarray):
    print("\nChannel summary:")
    for i, name in enumerate(CHANNEL_NAMES):
        sig = x[:, i]
        print(
            f"{i}: {name:15s} "
            f"mean={np.mean(sig): .4e}, "
            f"std={np.std(sig): .4e}, "
            f"min={np.min(sig): .4e}, "
            f"max={np.max(sig): .4e}"
        )


def compute_psd(signal: np.ndarray, fs: float, nperseg: int):
    signal = np.asarray(signal, dtype=float)
    signal = signal - np.mean(signal)

    nperseg = min(nperseg, len(signal))

    freqs, psd = welch(
        signal,
        fs=fs,
        window="hann",
        nperseg=nperseg,
        noverlap=nperseg // 2,
        detrend="constant",
        scaling="density",
    )

    return freqs, psd


def print_top_peaks(freqs, psd, max_freq=100.0, num_peaks=10):
    mask = (freqs > 0.5) & (freqs <= max_freq)

    f = freqs[mask]
    p = psd[mask]

    peaks, _ = find_peaks(p)

    if len(peaks) == 0:
        print("No peaks found.")
        return

    peak_freqs = f[peaks]
    peak_powers = p[peaks]

    order = np.argsort(peak_powers)[::-1]

    print(f"\nTop {num_peaks} PSD peaks below {max_freq} Hz:")
    for idx in order[:num_peaks]:
        print(f"  {peak_freqs[idx]:9.3f} Hz    PSD = {peak_powers[idx]:.4e}")


def plot_time_series(x, fs, output_dir, max_seconds=15.0):
    n = min(len(x), int(max_seconds * fs))
    t = np.arange(n) / fs

    plt.figure(figsize=(12, 6))

    for i, name in enumerate(CHANNEL_NAMES):
        if i == 3:
            continue  # currently unused
        plt.plot(t, x[:n, i], label=name)

    plt.xlabel("Time [s]")
    plt.ylabel("Acceleration [m/s²]")
    plt.title(f"sig00 time series, first {n / fs:.2f} seconds")
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()

    out = output_dir / "sig00_time_series.png"
    plt.savefig(out, dpi=200)
    print(f"Saved {out}")
    plt.close()


def plot_psd_all_channels(x, fs, output_dir, max_freq=100.0, nperseg=8192):
    plt.figure(figsize=(12, 6))

    for i, name in enumerate(CHANNEL_NAMES):
        if i == 3:
            continue  # currently unused

        freqs, psd = compute_psd(x[:, i], fs, nperseg=nperseg)

        mask = freqs <= max_freq
        plt.semilogy(freqs[mask], psd[mask], label=name)

        print(f"\n{name}")
        print_top_peaks(freqs, psd, max_freq=max_freq, num_peaks=10)

    plt.xlabel("Frequency [Hz]")
    plt.ylabel("PSD [(m/s²)²/Hz]")
    plt.title("sig00 accelerometer PSD")
    plt.grid(True, which="both", alpha=0.3)
    plt.legend()
    plt.tight_layout()

    out = output_dir / "sig00_psd_all_channels.png"
    plt.savefig(out, dpi=200)
    print(f"Saved {out}")
    plt.close()


def plot_psd_single_channel(x, fs, output_dir, channel, max_freq=100.0, nperseg=8192):
    name = CHANNEL_NAMES[channel]

    freqs, psd = compute_psd(x[:, channel], fs, nperseg=nperseg)

    plt.figure(figsize=(12, 6))
    mask = freqs <= max_freq
    plt.semilogy(freqs[mask], psd[mask])

    plt.xlabel("Frequency [Hz]")
    plt.ylabel("PSD [(m/s²)²/Hz]")
    plt.title(f"{name} PSD")
    plt.grid(True, which="both", alpha=0.3)
    plt.tight_layout()

    out = output_dir / f"{name}_psd.png"
    plt.savefig(out, dpi=200)
    print(f"Saved {out}")
    plt.close()

    print(f"\n{name}")
    print_top_peaks(freqs, psd, max_freq=max_freq, num_peaks=10)


def plot_psd_spectrogram(
    x,
    fs,
    output_dir,
    channel,
    max_freq=10.0,
    window_seconds=1.0,
    overlap=0.90,
    vmin=None,
    vmax=None,
):
    """
    Paper-style moving-window PSD plot.

    x:
        Array with shape (samples, 4)

    fs:
        Sample rate in Hz, e.g. 8000

    channel:
        0 = SC0_CH1
        1 = SC0_CH2
        2 = SC1_CH1
        3 = SC1_CH2_UNUSED

    max_freq:
        Maximum frequency shown on x-axis.

    window_seconds:
        Length of each moving PSD window.
        The paper often uses long windows, like 50 s.
        At 8 kHz, 50 s is 400,000 samples, which gives fine frequency resolution.

    overlap:
        Fractional overlap between windows.
        0.90 means each window overlaps the previous by 90%.

    vmin, vmax:
        Optional color scale limits for PSD.
        Leave as None at first.
    """

    name = CHANNEL_NAMES[channel]

    signal = np.asarray(x[:, channel], dtype=float)
    signal = signal - np.mean(signal)

    nperseg = int(window_seconds * fs)
    nperseg = min(nperseg, len(signal))

    noverlap = int(overlap * nperseg)
    noverlap = min(noverlap, nperseg - 1)

    freqs, times, Sxx = spectrogram(
        signal,
        fs=fs,
        window="hann",
        nperseg=nperseg,
        noverlap=noverlap,
        detrend="constant",
        scaling="density",
        mode="psd",
    )

    freq_mask = freqs <= max_freq

    freqs_plot = freqs[freq_mask]
    Sxx_plot = Sxx[freq_mask, :]

    # Avoid zeros because LogNorm cannot plot zero values
    Sxx_plot = np.maximum(Sxx_plot, 1e-30)

    if vmin is None:
        vmin = np.percentile(Sxx_plot, 5)

    if vmax is None:
        vmax = np.percentile(Sxx_plot, 99.8)

    fig, ax = plt.subplots(figsize=(8, 6))

    mesh = ax.pcolormesh(
        freqs_plot,
        times,
        Sxx_plot.T,
        shading="auto",
        cmap="jet",
        norm=LogNorm(vmin=vmin, vmax=vmax),
    )

    ax.set_xlabel("Frequency (Hz)", fontsize=14)
    ax.set_ylabel("Time (s)", fontsize=14)
    ax.set_title(name, fontsize=14)

    ax.set_xlim(0, max_freq)
    ax.set_ylim(times[0], times[-1])

    cbar = fig.colorbar(mesh, ax=ax)
    cbar.set_label("PSD of acceleration ((m/s²)²/Hz)", fontsize=12)

    fig.tight_layout()

    out = output_dir / f"{name}_paper_style_psd.png"
    fig.savefig(out, dpi=250)
    print(f"Saved {out}")

    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(
        description="Analyze sig00 FITS files from 2 signal conditioners in one stream."
    )

    parser.add_argument("fits_file", type=Path, help="Path to FITS file")
    parser.add_argument(
        "--fs",
        type=float,
        default=SAMPLE_RATE,
        help="Sample rate in Hz. Default: 8000",
    )
    parser.add_argument(
        "--max-freq",
        type=float,
        default=100.0,
        help="Maximum frequency to show in PSD plots. Default: 100 Hz",
    )
    parser.add_argument(
        "--spectrogram-max-freq",
        type=float,
        default=30.0,
        help="Maximum frequency to show in spectrograms. Default: 30 Hz",
    )
    parser.add_argument(
        "--nperseg",
        type=int,
        default=8192,
        help="Welch PSD segment length. Default: 8192",
    )
    parser.add_argument(
        "--channel",
        type=int,
        default=None,
        help="Optional single channel to analyze: 0=SC0_CH1, 1=SC0_CH2, 2=SC1_CH1, 3=SC1_CH2",
    )
    parser.add_argument(
        "--outdir",
        type=Path,
        default=None,
        help="Output directory. Default: same folder as FITS file",
    )

    args = parser.parse_args()

    if args.outdir is None:
        output_dir = args.fits_file.parent / f"{args.fits_file.stem}_analysis"
    else:
        output_dir = args.outdir

    output_dir.mkdir(parents=True, exist_ok=True)

    data = read_fits_data(args.fits_file)
    x = convert_to_time_by_channel(data)

    duration = len(x) / args.fs
    print(f"\nSamples:  {len(x)}")
    print(f"Duration: {duration:.3f} seconds")
    print(f"Fs:       {args.fs:.1f} Hz")

    summarize_channels(x)

    plot_time_series(x, args.fs, output_dir)

    if args.channel is None:
        plot_psd_all_channels(
            x,
            args.fs,
            output_dir,
            max_freq=args.max_freq,
            nperseg=args.nperseg,
        )

        for channel in [0, 1, 2]:
            plot_psd_spectrogram(
                x,
                args.fs,
                output_dir,
                channel,
                max_freq=10.0,
                window_seconds=1.0,
                overlap=0.90,
            )

    else:
        if not (0 <= args.channel <= 3):
            raise ValueError("Channel must be 0, 1, 2, or 3")

        plot_psd_single_channel(
            x,
            args.fs,
            output_dir,
            args.channel,
            max_freq=args.max_freq,
            nperseg=args.nperseg,
        )

        plot_psd_spectrogram(
            x,
            args.fs,
            output_dir,
            args.channel,
            max_freq=args.spectrogram_max_freq,
            window_seconds=1.0,
            overlap=0.90,
        )


if __name__ == "__main__":
    main()