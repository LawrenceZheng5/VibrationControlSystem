#!/usr/bin/env python3

import argparse
import csv
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


def compute_metrics(path, frame_index, main_index, sample_rate=None):
    """
    Compute quantitative metrics for Main index continuity.

    Main idea:
      diff = main_index[i+1] - main_index[i]

    Ideal:
      diff == 1

    Bad cases:
      diff > 1   means skipped/missing Main index values
      diff == 0  means repeated/stalled index
      diff < 0   means index went backwards
    """
    path = Path(path)

    if len(main_index) < 2:
        return {
            "file": path.name,
            "num_samples": len(main_index),
            "num_diffs": 0,
            "min_diff": None,
            "max_diff": None,
            "mean_diff": None,
            "std_diff": None,
            "num_bad_jumps": 0,
            "jump_rate_percent": None,
            "percent_continuous": None,
            "total_missing_samples": 0,
            "max_missing_in_one_jump": 0,
            "num_forward_jumps": 0,
            "num_duplicate_or_stall": 0,
            "num_backward_jumps": 0,
            "mean_bad_diff": None,
            "median_bad_diff": None,
            "mean_samples_between_jumps": None,
            "median_samples_between_jumps": None,
            "longest_clean_run_samples": None,
            "mean_seconds_between_jumps": None,
            "median_seconds_between_jumps": None,
        }

    diffs = np.diff(main_index)

    # Bad means anything other than the ideal continuous increment of 1.
    bad = np.where(diffs != 1)[0]

    # Forward jumps are the missing-data case.
    forward_jumps = np.where(diffs > 1)[0]

    # Duplicate/stall/backward cases are useful to track separately.
    duplicate_or_stall = np.where(diffs == 0)[0]
    backward_jumps = np.where(diffs < 0)[0]

    # For diff > 1, missing count is diff - 1.
    missing_counts = diffs[forward_jumps] - 1

    # Distance between bad jumps in units of diff-index samples.
    # Example: bad = [10, 20, 35] -> spacings = [10, 15]
    if len(bad) >= 2:
        jump_spacings = np.diff(bad)
        mean_samples_between_jumps = float(np.mean(jump_spacings))
        median_samples_between_jumps = float(np.median(jump_spacings))
    else:
        mean_samples_between_jumps = None
        median_samples_between_jumps = None

    # Longest clean run = largest number of consecutive good diffs between bad diffs.
    # This is useful because a file with rare jumps should have long clean sections.
    if len(bad) == 0:
        longest_clean_run_samples = len(diffs)
    else:
        # Include beginning and end edges.
        bad_with_edges = np.concatenate(([-1], bad, [len(diffs)]))
        clean_run_lengths = np.diff(bad_with_edges) - 1
        longest_clean_run_samples = int(np.max(clean_run_lengths))

    if sample_rate is not None and mean_samples_between_jumps is not None:
        mean_seconds_between_jumps = mean_samples_between_jumps / sample_rate
        median_seconds_between_jumps = median_samples_between_jumps / sample_rate
    else:
        mean_seconds_between_jumps = None
        median_seconds_between_jumps = None

    bad_diffs = diffs[bad]

    metrics = {
        "file": path.name,
        "num_samples": int(len(main_index)),
        "num_diffs": int(len(diffs)),
        "min_diff": int(np.min(diffs)),
        "max_diff": int(np.max(diffs)),
        "mean_diff": float(np.mean(diffs)),
        "std_diff": float(np.std(diffs)),

        # Overall continuity quality.
        "num_bad_jumps": int(len(bad)),
        "jump_rate_percent": 100.0 * len(bad) / len(diffs),
        "percent_continuous": 100.0 * np.sum(diffs == 1) / len(diffs),

        # Missing-data-specific metrics.
        "total_missing_samples": int(np.sum(missing_counts)) if len(missing_counts) > 0 else 0,
        "max_missing_in_one_jump": int(np.max(missing_counts)) if len(missing_counts) > 0 else 0,
        "num_forward_jumps": int(len(forward_jumps)),

        # Non-forward weirdness.
        "num_duplicate_or_stall": int(len(duplicate_or_stall)),
        "num_backward_jumps": int(len(backward_jumps)),

        # Bad jump severity.
        "mean_bad_diff": float(np.mean(bad_diffs)) if len(bad_diffs) > 0 else None,
        "median_bad_diff": float(np.median(bad_diffs)) if len(bad_diffs) > 0 else None,

        # Spacing between jumps.
        "mean_samples_between_jumps": mean_samples_between_jumps,
        "median_samples_between_jumps": median_samples_between_jumps,
        "longest_clean_run_samples": longest_clean_run_samples,

        # Optional time-based spacing, if sample rate is provided.
        "mean_seconds_between_jumps": mean_seconds_between_jumps,
        "median_seconds_between_jumps": median_seconds_between_jumps,
    }

    return metrics


def print_metrics(metrics):
    """
    Print a readable summary for one file.
    """
    print(f"Total samples: {metrics['num_samples']}")
    print(f"Total consecutive differences checked: {metrics['num_diffs']}")
    print(f"Min diff: {metrics['min_diff']}")
    print(f"Max diff: {metrics['max_diff']}")
    print(f"Number of non-continuous jumps: {metrics['num_bad_jumps']}")
    print(f"Jump rate: {metrics['jump_rate_percent']:.4f}%")
    print(f"Percent continuous: {metrics['percent_continuous']:.4f}%")
    print(f"Total missing samples: {metrics['total_missing_samples']}")
    print(f"Max missing in one jump: {metrics['max_missing_in_one_jump']}")
    print(f"Mean samples between jumps: {metrics['mean_samples_between_jumps']}")
    print(f"Longest clean run: {metrics['longest_clean_run_samples']} samples")


def print_bad_jump_table(main_index, diffs):
    """
    Print padded table of every bad jump.
    """
    bad = np.where(diffs != 1)[0]

    if len(bad) == 0:
        return

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


def save_diff_histogram(path, diffs, output_dir):
    """
    Save a histogram of diff values.

    This is useful because your scatter plot shows where jumps occur,
    while the histogram shows how often each jump size occurs.
    """
    path = Path(path)
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    output_file = output_dir / f"{path.stem}_diff_hist.png"

    unique_diffs, counts = np.unique(diffs, return_counts=True)

    plt.figure(figsize=(10, 5))
    plt.bar(unique_diffs, counts)
    plt.xlabel("Main index difference")
    plt.ylabel("Count")
    plt.title(f"Diff Histogram: {path.name}")
    plt.grid(True)

    plt.savefig(output_file, dpi=200, bbox_inches="tight")
    plt.close()

    return output_file


def analyze_file(path, output_dir=None, print_jumps=False, sample_rate=None):
    """
    Analyze one telemetry file:
      - compute differences between consecutive Main index values
      - calculate quantitative continuity metrics
      - optionally print every discontinuity
      - save plots
    """
    path = Path(path)

    print()
    print("=" * 90)
    print(f"Analyzing: {path}")
    print("=" * 90)

    frame_index, main_index = load_telemetry(path)

    if len(main_index) < 2:
        print("Not enough samples to compute consecutive differences.")
        return None

    diffs = np.diff(main_index)
    metrics = compute_metrics(path, frame_index, main_index, sample_rate=sample_rate)

    print_metrics(metrics)

    if print_jumps:
        print_bad_jump_table(main_index, diffs)

    # Decide where to save the output plots.
    # If output_dir is provided, save all plots there.
    # Otherwise, save next to the input text file inside a plots directory.
    if output_dir is not None:
        output_dir = Path(output_dir)
    else:
        output_dir = path.parent / "plots"

    output_dir.mkdir(parents=True, exist_ok=True)

    # Scatter plot of consecutive Main index differences.
    output_file = output_dir / f"{path.stem}.png"

    plt.figure(figsize=(12, 5))
    plt.plot(diffs, marker=".", linestyle="none")
    plt.axhline(1, linestyle="--", label="Expected diff = 1")

    plt.xlabel("Consecutive sample number")
    plt.ylabel("Main index difference")
    plt.title(f"Main Index Difference: {path.name}")
    plt.legend()
    plt.grid(True)

    plt.savefig(output_file, dpi=200, bbox_inches="tight")
    plt.close()  # Close the figure to free memory.

    print(f"Saved plot to: {output_file}")

    # Histogram of jump sizes.
    hist_file = save_diff_histogram(path, diffs, output_dir)
    print(f"Saved diff histogram to: {hist_file}")

    return metrics


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


def write_summary_csv(metrics_list, output_csv):
    """
    Write one CSV row per telemetry file so runs can be compared quantitatively.
    """
    output_csv = Path(output_csv)
    output_csv.parent.mkdir(parents=True, exist_ok=True)

    # Remove None entries from files that failed or had too few samples.
    metrics_list = [m for m in metrics_list if m is not None]

    if len(metrics_list) == 0:
        print("No metrics to write.")
        return

    fieldnames = list(metrics_list[0].keys())

    with output_csv.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(metrics_list)

    print()
    print(f"Saved summary CSV to: {output_csv}")


def print_comparison_table(metrics_list):
    """
    Print a compact comparison table sorted by jump rate.
    Lower jump rate and lower total_missing_samples are better.
    """
    metrics_list = [m for m in metrics_list if m is not None]

    if len(metrics_list) == 0:
        return

    metrics_list = sorted(metrics_list, key=lambda m: m["jump_rate_percent"])

    def mean_metric(key):
        """
        Average one metric across all files.

        Skips None values so files with missing metrics do not crash the script.
        """
        values = [m[key] for m in metrics_list if m[key] is not None]

        if len(values) == 0:
            return None

        return float(np.mean(values))

    print()
    print("=" * 120)
    print("Comparison summary, sorted by jump rate")
    print("=" * 120)

    print(
        f"{'file':<35}"
        f"{'bad':>8}"
        f"{'jump %':>10}"
        f"{'missing':>10}"
        f"{'max diff':>10}"
        f"{'mean gap':>12}"
        f"{'longest clean':>16}"
    )
    print("-" * 120)

    for m in metrics_list:
        mean_gap = m["mean_samples_between_jumps"]
        mean_gap_str = f"{mean_gap:.1f}" if mean_gap is not None else "N/A"

        print(
            f"{m['file']:<35}"
            f"{m['num_bad_jumps']:>8}"
            f"{m['jump_rate_percent']:>10.4f}"
            f"{m['total_missing_samples']:>10}"
            f"{m['max_diff']:>10}"
            f"{mean_gap_str:>12}"
            f"{m['longest_clean_run_samples']:>16}"
        )

    # Compute averages across all files in the comparison table.
    avg_bad = mean_metric("num_bad_jumps")
    avg_jump_percent = mean_metric("jump_rate_percent")
    avg_missing = mean_metric("total_missing_samples")
    avg_max_diff = mean_metric("max_diff")
    avg_mean_gap = mean_metric("mean_samples_between_jumps")
    avg_longest_clean = mean_metric("longest_clean_run_samples")

    avg_mean_gap_str = f"{avg_mean_gap:.1f}" if avg_mean_gap is not None else "N/A"

    print("-" * 120)
    print(
        f"{'AVERAGE':<35}"
        f"{avg_bad:>8.1f}"
        f"{avg_jump_percent:>10.4f}"
        f"{avg_missing:>10.1f}"
        f"{avg_max_diff:>10.1f}"
        f"{avg_mean_gap_str:>12}"
        f"{avg_longest_clean:>16.1f}"
    )


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
        "--print-jumps",
        action="store_true",
        help="Print every non-continuous jump to the console.",
    )

    parser.add_argument(
        "--summary-csv",
        default=None,
        help="Optional filename for the metrics CSV. Default: save continuity_summary.csv in the plot output directory.",
    )

    parser.add_argument(
        "--sample-rate",
        type=float,
        default=None,
        help="Optional sample rate in Hz. Used to convert jump spacing from samples to seconds.",
    )

    args = parser.parse_args()

    files = get_input_files(args.input, recursive=args.recursive)

    if len(files) == 0:
        print(f"No .txt files found in: {args.input}")
        return

    print(f"Found {len(files)} file(s) to analyze.")

    metrics_list = []

    for path in files:
        try:
            metrics = analyze_file(
                path,
                output_dir=args.output_dir,
                print_jumps=args.print_jumps,
                sample_rate=args.sample_rate,
            )
            metrics_list.append(metrics)
        except Exception as e:
            print()
            print("=" * 80)
            print(f"Error analyzing {path}")
            print("=" * 80)
            print(e)

    print_comparison_table(metrics_list)

    # Decide where the CSV should go.
    if args.summary_csv is not None:
        output_csv = Path(args.summary_csv)
    elif args.output_dir is not None:
        output_csv = Path(args.output_dir) / "continuity_summary.csv"
    else:
        input_path = Path(args.input)
        if input_path.is_dir():
            output_csv = input_path / "plots" / "continuity_summary.csv"
        else:
            output_csv = input_path.parent / "plots" / "continuity_summary.csv"

    write_summary_csv(metrics_list, output_csv)


if __name__ == "__main__":
    main()