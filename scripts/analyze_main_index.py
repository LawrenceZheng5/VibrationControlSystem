#!/usr/bin/env python3

import argparse
from pathlib import Path

import numpy as np
import matplotlib.pyplot as plt


def load_telemetry(path):
    """
    Load one telemetry timing text file.

    Expected columns:
      col1: datacube frame index
      col2: Main index
      col3+: timing / counter metadata

    Lines starting with "#" are skipped automatically by np.loadtxt.
    """
    data = np.loadtxt(path, comments="#")

    # If the file only contains one data row, np.loadtxt returns a 1D array.
    # Reshape it so the rest of the code can still index columns normally.
    if data.ndim == 1:
        data = data.reshape(1, -1)

    frame_index = data[:, 0].astype(int)
    main_index = data[:, 1].astype(int)

    return frame_index, main_index


def analyze_file(path, output_dir=None, show_plot=False):
    """
    Analyze one telemetry file:
      - compute differences between consecutive Main index values
      - print any discontinuities
      - save a plot of the differences
    """
    path = Path(path)

    print()
    print("=" * 80)
    print(f"Analyzing: {path}")
    print("=" * 80)

    frame_index, main_index = load_telemetry(path)

    if len(main_index) < 2:
        print("Not enough samples to compute consecutive differences.")
        return

    diffs = np.diff(main_index)

    # Ideal case: every consecutive Main index difference should be exactly 1.
    bad = np.where(diffs != 1)[0]

    print(f"Total samples: {len(main_index)}")
    print(f"Total consecutive differences checked: {len(diffs)}")
    print(f"Min diff: {diffs.min()}")
    print(f"Max diff: {diffs.max()}")
    print(f"Number of non-continuous jumps: {len(bad)}")

    if len(bad) > 0:
        print("\nNon-continuous jumps:")
        print(
            f"{'row range':<20}"
            f"{'main range':<30}"
            f"{'diff':>8}"
            f"{'missing':>10}"
        )
        print("-" * 70)

        for i in bad:
            # If diff is 3, then two index values were skipped.
            # If diff is less than or equal to 1, treat missing count as 0.
            missing_count = max(diffs[i] - 1, 0)

            print(
                f"{f'{i}->{i+1}':<20}"
                f"{f'{main_index[i]}->{main_index[i+1]}':<30}"
                f"{diffs[i]:>8}"
                f"{missing_count:>10}"
            )

    # Decide where to save the output plot.
    # If output_dir is provided, save all plots there.
    # Otherwise, save next to the input text file.
    if output_dir is not None:
        output_dir = Path(output_dir)
        output_dir.mkdir(parents=True, exist_ok=True)
        output_file = output_dir / f"{path.stem}.png"
    else:
        output_file = path.with_suffix(".png")

    plt.figure(figsize=(12, 5))
    plt.plot(diffs, marker=".", linestyle="none")
    plt.axhline(1, linestyle="--", label="Expected diff = 1")

    plt.xlabel("Consecutive sample number")
    plt.ylabel("Main index difference")
    plt.title(f"Main Index Difference: {path.name}")
    plt.legend()
    plt.grid(True)

    plt.savefig(output_file, dpi=200, bbox_inches="tight")
    print(f"\nSaved plot to: {output_file}")

    if show_plot:
        plt.show()
    else:
        # Important when processing many files so figures do not pile up in memory.
        plt.close()


def get_input_files(input_path, recursive=False):
    """
    Return a list of telemetry text files to analyze.

    If input_path is a file:
      analyze just that file.

    If input_path is a directory:
      analyze all .txt files in that directory.
    """
    input_path = Path(input_path)

    if input_path.is_file():
        return [input_path]

    if input_path.is_dir():
        if recursive:
            return sorted(input_path.rglob("*.txt"))
        return sorted(input_path.glob("*.txt"))

    raise FileNotFoundError(f"Input path does not exist: {input_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Analyze telemetry Main index continuity for one file or all text files in a directory."
    )

    parser.add_argument(
        "input",
        help="Telemetry timing text file or directory containing .txt files",
    )

    parser.add_argument(
        "--output-dir",
        default=None,
        help="Optional directory where plots should be saved. Default: save next to each input file.",
    )

    parser.add_argument(
        "--recursive",
        action="store_true",
        help="If input is a directory, also search subdirectories for .txt files.",
    )

    parser.add_argument(
        "--show",
        action="store_true",
        help="Show each plot interactively. Not recommended for many files.",
    )

    args = parser.parse_args()

    files = get_input_files(args.input, recursive=args.recursive)

    if len(files) == 0:
        print(f"No .txt files found in: {args.input}")
        return

    print(f"Found {len(files)} file(s) to analyze.")

    for path in files:
        try:
            analyze_file(
                path,
                output_dir=args.output_dir,
                show_plot=args.show,
            )
        except Exception as e:
            print()
            print("=" * 80)
            print(f"Error analyzing {path}")
            print("=" * 80)
            print(e)


if __name__ == "__main__":
    main()