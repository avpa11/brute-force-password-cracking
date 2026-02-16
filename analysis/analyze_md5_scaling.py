import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
from pathlib import Path

plt.rcParams.update({
    'font.size': 11,
    'axes.titlesize': 13,
    'axes.labelsize': 12,
    'xtick.labelsize': 10,
    'ytick.labelsize': 10,
    'legend.fontsize': 10,
    'figure.dpi': 150,
    'savefig.dpi': 300,
    'savefig.bbox': 'tight',
})

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent
OUTPUT_DIR = ROOT_DIR / 'results'
CSV_FILE = OUTPUT_DIR / 'md5_thread_experiment.csv'


def load_data():
    df = pd.read_csv(CSV_FILE)
    df['Worker_ms'] = pd.to_numeric(df['Worker_ms'], errors='coerce')
    df['Total_ms'] = pd.to_numeric(df['Total_ms'], errors='coerce')
    df = df.dropna()
    return df


def print_summary_statistics(df):
    """Print mean, std, min, max for each thread count."""
    print("\n" + "=" * 72)
    print("  SUMMARY STATISTICS — MD5 Worker Time")
    print("=" * 72)
    header = f"{'Threads':>7} {'Trials':>6} {'Mean (ms)':>12} {'Std (ms)':>10} {'Min (ms)':>10} {'Max (ms)':>10}"
    print(header)
    print("-" * 72)

    for t in sorted(df['Threads'].unique()):
        td = df[df['Threads'] == t]['Worker_ms']
        print(f"{t:>7} {len(td):>6} {td.mean():>12.3f} {td.std():>10.3f} "
              f"{td.min():>10.3f} {td.max():>10.3f}")
    print("-" * 72)


def plot_variability_boxplot(df):
    """Box plot with individual data points showing trial-to-trial variability."""
    threads = sorted(df['Threads'].unique())

    fig, ax = plt.subplots(figsize=(9, 6))

    data = [df[df['Threads'] == t]['Worker_ms'].values for t in threads]
    positions = list(range(1, len(threads) + 1))
    labels = [str(t) for t in threads]

    bp = ax.boxplot(data, positions=positions, patch_artist=True, widths=0.5,
                    medianprops=dict(color='black', linewidth=2),
                    flierprops=dict(marker='o', markersize=5))

    colors = ['#4C72B0', '#55A868', '#C44E52', '#8172B2', '#CCB974']
    for patch, c in zip(bp['boxes'], colors[:len(threads)]):
        patch.set_facecolor(c)
        patch.set_alpha(0.6)

    # Overlay individual points
    for i, (d, c) in enumerate(zip(data, colors)):
        jitter = np.random.default_rng(42).normal(0, 0.06, size=len(d))
        ax.scatter(np.full_like(d, positions[i]) + jitter, d, color=c,
                   edgecolor='black', linewidth=0.5, s=40, zorder=5, alpha=0.85)

    ax.set_xticklabels(labels)
    ax.set_xlabel('Number of Threads')
    ax.set_ylabel('Worker Crack Time (ms)')
    ax.set_title('MD5 Trial-to-Trial Variability by Thread Count\n'
                 '(box = IQR, whiskers = 1.5×IQR, dots = individual trials)')
    ax.grid(axis='y', alpha=0.3, linestyle='--')
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)

    plt.tight_layout()
    path = OUTPUT_DIR / 'md5_variability_boxplot.png'
    plt.savefig(path)
    plt.close()
    print(f"Saved: {path}")


def compute_amdahl_prediction(df):
    """
    Amdahl's Law Model
    -------------------
    Speedup(N) = 1 / (f + (1 - f) / N)

    where f = serial fraction, N = thread count.

    We estimate f from the measured 1-thread and 4-thread data:
        S(4) = T(1) / T(4)
        f = (1/S(4) - 1/4) / (1 - 1/4)
    """
    t1_mean = df[df['Threads'] == 1]['Worker_ms'].mean()
    t4_mean = df[df['Threads'] == 4]['Worker_ms'].mean()
    s4 = t1_mean / t4_mean

    # Solve for serial fraction f
    f = (1.0 / s4 - 1.0 / 4) / (1.0 - 1.0 / 4)
    f = max(0.0, min(1.0, f))  # clamp to [0,1]

    print(f"\n{'=' * 72}")
    print("  AMDAHL'S LAW MODEL")
    print(f"{'=' * 72}")
    print(f"  T(1) mean  = {t1_mean:.3f} ms")
    print(f"  T(4) mean  = {t4_mean:.3f} ms")
    print(f"  S(4)       = {s4:.4f}x")
    print(f"  Serial fraction (f) = {f:.6f}")
    print()

    # Predict for all thread counts
    print(f"  {'Threads':>7} {'Predicted Speedup':>18} {'Predicted Time (ms)':>20}")
    print(f"  {'-'*50}")
    predictions = {}
    for n in [1, 2, 3, 4, 10]:
        speedup = 1.0 / (f + (1.0 - f) / n)
        predicted_time = t1_mean / speedup
        predictions[n] = {'speedup': speedup, 'time': predicted_time}
        print(f"  {n:>7} {speedup:>18.4f}x {predicted_time:>20.3f}")

    print()
    print(f"  PREDICTION for 10 threads:")
    p10 = predictions[10]
    print(f"    Expected speedup:  {p10['speedup']:.4f}x")
    print(f"    Expected time:     {p10['time']:.3f} ms")
    print()
    print(f"  Model explanation:")
    print(f"    With serial fraction f = {f:.6f}, Amdahl's Law predicts that")
    print(f"    adding more threads yields diminishing returns.")
    if f > 0:
        print(f"    The maximum theoretical speedup is {1/f:.2f}x (as N -> infinity).")
    else:
        print(f"    With f ~= 0, the workload is nearly perfectly parallel.")
    print(f"    At 10 threads we expect ~{p10['speedup']:.2f}x speedup.")
    print(f"    The serial portion includes thread creation, atomic operations,")
    print(f"    and the password-found check, which cannot be parallelized.")
    print(f"    However at 10 threads, contention and OS scheduling may cause")
    print(f"    performance to diverge from this ideal prediction.")
    print(f"{'=' * 72}")

    return f, t1_mean, predictions


def plot_scaling_with_prediction(df, serial_fraction, t1_mean, predictions):
    """Graph measured vs predicted performance, including 10-thread result."""
    threads_measured = sorted(df['Threads'].unique())

    # Measured data
    means = [df[df['Threads'] == t]['Worker_ms'].mean() for t in threads_measured]
    stds = [df[df['Threads'] == t]['Worker_ms'].std() for t in threads_measured]
    speedups_measured = [t1_mean / m for m in means]

    # Amdahl curve (smooth)
    n_smooth = np.linspace(1, max(12, max(threads_measured) + 2), 100)
    speedup_amdahl = 1.0 / (serial_fraction + (1.0 - serial_fraction) / n_smooth)

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6))

    # --- Left panel: Worker time ---
    ax1.errorbar(threads_measured, means, yerr=stds, marker='o', capsize=5,
                 color='#4C72B0', linewidth=2, markersize=8, label='Measured', zorder=5)

    pred_times = [t1_mean / s for s in speedup_amdahl]
    ax1.plot(n_smooth, pred_times, '--', color='#C44E52', linewidth=2,
             label=f'Amdahl prediction (f={serial_fraction:.4f})', alpha=0.8)

    # Mark the 10-thread prediction point
    if 10 in predictions:
        ax1.scatter([10], [predictions[10]['time']], marker='x', color='red',
                    s=150, linewidths=3, zorder=6, label='10-thread prediction')

    ax1.set_xlabel('Number of Threads')
    ax1.set_ylabel('Worker Crack Time (ms)')
    ax1.set_title('MD5 Worker Time vs Thread Count')
    ax1.legend()
    ax1.grid(alpha=0.3, linestyle='--')
    ax1.spines['top'].set_visible(False)
    ax1.spines['right'].set_visible(False)
    ax1.set_xticks(threads_measured)

    # --- Right panel: Speedup ---
    ax2.plot(threads_measured, speedups_measured, 'o-', color='#4C72B0',
             linewidth=2, markersize=8, label='Measured', zorder=5)
    ax2.plot(n_smooth, speedup_amdahl, '--', color='#C44E52', linewidth=2,
             label=f'Amdahl (f={serial_fraction:.4f})', alpha=0.8)
    ax2.plot([1, max(n_smooth)], [1, max(n_smooth)], 'k:', alpha=0.3, label='Ideal (linear)')

    if 10 in predictions:
        ax2.scatter([10], [predictions[10]['speedup']], marker='x', color='red',
                    s=150, linewidths=3, zorder=6, label='10-thread prediction')

    ax2.set_xlabel('Number of Threads')
    ax2.set_ylabel('Speedup (×)')
    ax2.set_title('MD5 Parallel Speedup: Measured vs Predicted')
    ax2.legend(loc='upper left')
    ax2.grid(alpha=0.3, linestyle='--')
    ax2.spines['top'].set_visible(False)
    ax2.spines['right'].set_visible(False)
    ax2.set_xticks(threads_measured)

    plt.tight_layout()
    path = OUTPUT_DIR / 'md5_scaling_prediction.png'
    plt.savefig(path)
    plt.close()
    print(f"Saved: {path}")


def print_10thread_comparison(df, predictions):
    """Compare 10-thread measured result against Amdahl prediction."""
    t10 = df[df['Threads'] == 10]
    if t10.empty:
        print("\nNo 10-thread data found — skipping comparison.")
        return

    t1_mean = df[df['Threads'] == 1]['Worker_ms'].mean()
    measured_mean = t10['Worker_ms'].mean()
    measured_speedup = t1_mean / measured_mean
    predicted_speedup = predictions[10]['speedup']
    predicted_time = predictions[10]['time']
    error_pct = (measured_mean - predicted_time) / predicted_time * 100

    print(f"\n{'=' * 72}")
    print("  10-THREAD RESULTS vs PREDICTION")
    print(f"{'=' * 72}")
    print(f"  {'Metric':<25} {'Predicted':>15} {'Measured':>15} {'Error':>10}")
    print(f"  {'-'*65}")
    print(f"  {'Worker time (ms)':<25} {predicted_time:>15.3f} {measured_mean:>15.3f} {error_pct:>+9.1f}%")
    print(f"  {'Speedup':<25} {predicted_speedup:>15.4f}x {measured_speedup:>14.4f}x")
    print()

    # Analysis of divergence
    if abs(error_pct) < 5:
        verdict = "closely matches"
    elif error_pct > 0:
        verdict = "is SLOWER than"
    else:
        verdict = "is FASTER than"

    print(f"  The measured 10-thread result {verdict} the Amdahl prediction.")
    print()

    if error_pct > 5:
        print("  Likely causes for worse-than-predicted performance:")
        print("    - Thread contention on atomic variables (g_tested, g_found)")
        print("    - Cache line bouncing between cores for shared atomics")
        print("    - OS scheduling overhead with more threads than physical cores")
        print("    - Memory bandwidth saturation from parallel crypt_r() calls")
        print("    - Context switching overhead when threads > CPU cores")
    elif error_pct < -5:
        print("  Likely causes for better-than-predicted performance:")
        print("    - Early termination: password found sooner with more threads")
        print("      searching different parts of the space simultaneously")
        print("    - CPU cache effects from smaller per-thread working sets")
        print("    - The serial fraction estimate from 4 threads was conservative")
    else:
        print("  The Amdahl model with the estimated serial fraction provides")
        print("  a good fit for MD5, which has low per-hash computation cost")
        print("  and minimal contention on the shared atomic counters.")

    print(f"{'=' * 72}")


def main():
    print("\n" + "=" * 72)
    print("  MD5 THREAD SCALING ANALYSIS")
    print("=" * 72)

    df = load_data()
    print(f"\nLoaded {len(df)} data points")
    print(f"Thread counts: {sorted(df['Threads'].unique())}")
    print(f"Trials per config: {df.groupby('Threads').size().iloc[0]}")

    # 1. Summary statistics
    print_summary_statistics(df)

    # 2. Variability box plot
    plot_variability_boxplot(df)

    # 3. Amdahl prediction (using 1-4 thread data)
    serial_fraction, t1_mean, predictions = compute_amdahl_prediction(df)

    # 4. Compare 10-thread measured vs predicted
    print_10thread_comparison(df, predictions)

    # 5. Combined scaling + prediction graph
    plot_scaling_with_prediction(df, serial_fraction, t1_mean, predictions)

    print("\n" + "=" * 72)
    print("  DONE — Output files:")
    print("=" * 72)
    for p in sorted(OUTPUT_DIR.glob('md5_*.png')):
        print(f"  {p.name}")
    print()


if __name__ == '__main__':
    main()
