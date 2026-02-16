import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np
import sys
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

ALGO_ORDER = ['MD5', 'SHA256', 'SHA512', 'bcrypt', 'yescrypt']
ALGO_COLORS = {
    'MD5': '#4C72B0', 'SHA256': '#55A868', 'SHA512': '#C44E52',
    'bcrypt': '#8172B2', 'yescrypt': '#CCB974',
}
COMP_COLORS = {
    'Parse': '#1f77b4', 'Dispatch': '#ff7f0e',
    'Worker': '#2ca02c', 'Return': '#d62728',
}


def load_results(csv_file):
    try:
        df = pd.read_csv(csv_file)
    except FileNotFoundError:
        print(f"Error: '{csv_file}' not found. Run experiments first.")
        sys.exit(1)

    # Handle both old (no Threads column) and new CSV formats
    if 'Threads' not in df.columns:
        df.insert(1, 'Threads', 1)
    if 'Heartbeats' not in df.columns:
        df['Heartbeats'] = 0
    if 'Found' not in df.columns:
        df['Found'] = 1

    # Ensure correct types
    for col in ['Parse_ms', 'Dispatch_ms', 'Worker_ms', 'Return_ms', 'Total_ms']:
        df[col] = pd.to_numeric(df[col], errors='coerce')
    df['Threads'] = pd.to_numeric(df['Threads'], errors='coerce').fillna(1).astype(int)

    # Order algorithms consistently
    algo_cat = pd.CategoricalDtype(
        categories=[a for a in ALGO_ORDER if a in df['Algorithm'].unique()],
        ordered=True)
    df['Algorithm'] = df['Algorithm'].astype(algo_cat)
    df = df.sort_values(['Algorithm', 'Threads', 'Trial']).reset_index(drop=True)
    return df


def fmt_time(ms):
    """Format milliseconds into human-readable string."""
    if ms < 1:
        return f"{ms*1000:.1f} us"
    if ms < 1000:
        return f"{ms:.3f} ms"
    return f"{ms/1000:.3f} s"


# ── TABLE 1: Runtime by Algorithm (single-threaded) ────────────────────────

def print_runtime_table(df, output_dir):
    st = df[df['Threads'] == 1].copy()
    if st.empty:
        st = df.copy()

    stats = st.groupby('Algorithm', observed=True).agg(
        n=('Total_ms', 'size'),
        parse_mean=('Parse_ms', 'mean'), parse_std=('Parse_ms', 'std'),
        dispatch_mean=('Dispatch_ms', 'mean'), dispatch_std=('Dispatch_ms', 'std'),
        worker_mean=('Worker_ms', 'mean'), worker_std=('Worker_ms', 'std'),
        return_mean=('Return_ms', 'mean'), return_std=('Return_ms', 'std'),
        total_mean=('Total_ms', 'mean'), total_std=('Total_ms', 'std'),
        total_min=('Total_ms', 'min'), total_max=('Total_ms', 'max'),
    )

    hdr  = f"{'Algorithm':<10} {'n':>3}  {'Parse (ms)':>14}  {'Dispatch (ms)':>14}  {'Worker (s)':>14}  {'Return (ms)':>14}  {'Total (s)':>14}"
    sep  = "-" * len(hdr)

    lines = [sep, "Table 1: Single-Threaded Runtime by Algorithm", sep, hdr, sep]
    for algo in stats.index:
        r = stats.loc[algo]
        lines.append(
            f"{algo:<10} {int(r.n):>3}  "
            f"{r.parse_mean:>7.3f}+/-{r.parse_std:>5.3f}  "
            f"{r.dispatch_mean:>7.4f}+/-{r.dispatch_std:>5.4f}  "
            f"{r.worker_mean/1000:>7.3f}+/-{r.worker_std/1000:>5.3f}  "
            f"{r.return_mean:>7.4f}+/-{r.return_std:>5.4f}  "
            f"{r.total_mean/1000:>7.3f}+/-{r.total_std/1000:>5.3f}"
        )
    lines.append(sep)
    table_text = "\n".join(lines)
    print(table_text)

    (output_dir / 'table_runtime.txt').write_text(table_text + "\n")
    print(f"\nSaved: {output_dir / 'table_runtime.txt'}")
    return stats


# ── TABLE 2: Component percentages ─────────────────────────────────────────

def print_component_table(df, output_dir):
    st = df[df['Threads'] == 1].copy()
    if st.empty:
        st = df.copy()

    hdr  = f"{'Algorithm':<10} {'Parse %':>9} {'Dispatch %':>11} {'Worker %':>9} {'Return %':>9} {'Overhead (ms)':>14}"
    sep  = "-" * len(hdr)
    lines = [sep, "Table 2: Runtime Component Breakdown (% of Total)", sep, hdr, sep]

    for algo in st['Algorithm'].cat.categories:
        ad = st[st['Algorithm'] == algo]
        if ad.empty:
            continue
        total = ad['Total_ms'].mean()
        p = ad['Parse_ms'].mean() / total * 100
        d = ad['Dispatch_ms'].mean() / total * 100
        w = ad['Worker_ms'].mean() / total * 100
        r = ad['Return_ms'].mean() / total * 100
        overhead = ad['Parse_ms'].mean() + ad['Dispatch_ms'].mean() + ad['Return_ms'].mean()
        lines.append(f"{algo:<10} {p:>8.4f}% {d:>10.5f}% {w:>8.4f}% {r:>8.5f}% {overhead:>13.3f}")
    lines.append(sep)
    table_text = "\n".join(lines)
    print("\n" + table_text)

    (output_dir / 'table_components.txt').write_text(table_text + "\n")
    print(f"\nSaved: {output_dir / 'table_components.txt'}")


# ── TABLE 3: Thread scaling (if multi-thread data exists) ──────────────────

def print_thread_table(df, output_dir):
    thread_counts = sorted(df['Threads'].unique())
    if len(thread_counts) < 2:
        return

    hdr  = f"{'Algorithm':<10} {'Threads':>7} {'Worker (s)':>14} {'Total (s)':>14} {'Speedup':>8}"
    sep  = "-" * len(hdr)
    lines = [sep, "Table 3: Multi-Thread Scaling", sep, hdr, sep]

    for algo in df['Algorithm'].cat.categories:
        ad = df[df['Algorithm'] == algo]
        if ad.empty:
            continue
        base = ad[ad['Threads'] == 1]['Worker_ms'].mean()
        for t in thread_counts:
            td = ad[ad['Threads'] == t]
            if td.empty:
                continue
            wm = td['Worker_ms'].mean()
            tm = td['Total_ms'].mean()
            speedup = base / wm if wm > 0 else 0
            lines.append(
                f"{algo:<10} {t:>7}  "
                f"{wm/1000:>7.3f}+/-{td['Worker_ms'].std()/1000:>5.3f}  "
                f"{tm/1000:>7.3f}+/-{td['Total_ms'].std()/1000:>5.3f}  "
                f"{speedup:>7.2f}x"
            )
        lines.append("")
    lines.append(sep)
    table_text = "\n".join(lines)
    print("\n" + table_text)

    (output_dir / 'table_threads.txt').write_text(table_text + "\n")
    print(f"\nSaved: {output_dir / 'table_threads.txt'}")


# ── GRAPH 1: Total end-to-end runtime by algorithm ─────────────────────────

def plot_total_runtime(df, output_dir):
    st = df[df['Threads'] == 1].copy()
    if st.empty:
        st = df.copy()

    fig, ax = plt.subplots(figsize=(8, 5))

    algos = [a for a in ALGO_ORDER if a in st['Algorithm'].unique()]
    means = [st[st['Algorithm'] == a]['Total_ms'].mean() / 1000 for a in algos]
    stds  = [st[st['Algorithm'] == a]['Total_ms'].std()  / 1000 for a in algos]
    colors = [ALGO_COLORS.get(a, '#999999') for a in algos]

    bars = ax.bar(algos, means, yerr=stds, capsize=6, color=colors,
                  edgecolor='black', linewidth=0.5, alpha=0.85, width=0.55)

    for bar, m, s in zip(bars, means, stds):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + s + 0.3,
                f'{m:.2f} s', ha='center', va='bottom', fontsize=10, fontweight='bold')

    ax.set_xlabel('Hash Algorithm')
    ax.set_ylabel('Total Runtime (seconds)')
    ax.set_title('Total End-to-End Runtime by Algorithm\n(single-threaded, error bars = 1 SD)')
    ax.set_ylim(bottom=0)
    ax.grid(axis='y', alpha=0.3, linestyle='--')
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)

    plt.tight_layout()
    plt.savefig(output_dir / 'total_runtime.png')
    plt.close()
    print(f"Saved: {output_dir / 'total_runtime.png'}")


# ── GRAPH 2: Component breakdown (stacked + percentage inset) ──────────────

def plot_component_breakdown(df, output_dir):
    st = df[df['Threads'] == 1].copy()
    if st.empty:
        st = df.copy()

    algos = [a for a in ALGO_ORDER if a in st['Algorithm'].unique()]

    parse_ms    = [st[st['Algorithm']==a]['Parse_ms'].mean()    for a in algos]
    dispatch_ms = [st[st['Algorithm']==a]['Dispatch_ms'].mean() for a in algos]
    worker_ms   = [st[st['Algorithm']==a]['Worker_ms'].mean()   for a in algos]
    return_ms   = [st[st['Algorithm']==a]['Return_ms'].mean()   for a in algos]
    total_ms    = [p+d+w+r for p,d,w,r in zip(parse_ms, dispatch_ms, worker_ms, return_ms)]

    # Convert to seconds for the main chart
    parse_s    = [v/1000 for v in parse_ms]
    dispatch_s = [v/1000 for v in dispatch_ms]
    worker_s   = [v/1000 for v in worker_ms]
    return_s   = [v/1000 for v in return_ms]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5.5),
                                    gridspec_kw={'width_ratios': [2, 1.2]})

    # Left: stacked bar (absolute times in seconds)
    x = np.arange(len(algos))
    w = 0.5
    b1 = ax1.bar(x, parse_s,    w, label='Parse',    color=COMP_COLORS['Parse'])
    b2 = ax1.bar(x, dispatch_s, w, bottom=parse_s,    label='Dispatch', color=COMP_COLORS['Dispatch'])
    bottom2 = [p+d for p,d in zip(parse_s, dispatch_s)]
    b3 = ax1.bar(x, worker_s,   w, bottom=bottom2,    label='Worker',   color=COMP_COLORS['Worker'])
    bottom3 = [b+wk for b,wk in zip(bottom2, worker_s)]
    b4 = ax1.bar(x, return_s,   w, bottom=bottom3,    label='Return',   color=COMP_COLORS['Return'])

    ax1.set_xticks(x)
    ax1.set_xticklabels(algos)
    ax1.set_ylabel('Time (seconds)')
    ax1.set_title('Runtime Breakdown by Component')
    ax1.legend(loc='upper left', framealpha=0.9)
    ax1.grid(axis='y', alpha=0.3, linestyle='--')
    ax1.spines['top'].set_visible(False)
    ax1.spines['right'].set_visible(False)

    # Right: grouped bar showing overhead components on their own scale
    # (since worker dominates, overhead is invisible in stacked chart)
    overhead = {
        'Parse':    parse_ms,
        'Dispatch': dispatch_ms,
        'Return':   return_ms,
    }
    n_groups = len(algos)
    n_bars = len(overhead)
    bar_w = 0.22
    offsets = np.arange(n_bars) * bar_w - (n_bars-1)*bar_w/2

    for i, (comp, vals) in enumerate(overhead.items()):
        ax2.bar(x + offsets[i], vals, bar_w, label=comp,
                color=COMP_COLORS[comp], edgecolor='black', linewidth=0.3, alpha=0.85)

    ax2.set_xticks(x)
    ax2.set_xticklabels(algos)
    ax2.set_ylabel('Time (ms)')
    ax2.set_title('Overhead Components (zoomed)')
    ax2.legend(framealpha=0.9)
    ax2.grid(axis='y', alpha=0.3, linestyle='--')
    ax2.spines['top'].set_visible(False)
    ax2.spines['right'].set_visible(False)

    plt.tight_layout()
    plt.savefig(output_dir / 'component_breakdown.png')
    plt.close()
    print(f"Saved: {output_dir / 'component_breakdown.png'}")


# ── GRAPH 3: Trial-to-trial variability (box + strip plots) ───────────────

def plot_variability(df, output_dir):
    st = df[df['Threads'] == 1].copy()
    if st.empty:
        st = df.copy()

    algos = [a for a in ALGO_ORDER if a in st['Algorithm'].unique()]

    fig, axes = plt.subplots(1, 3, figsize=(15, 5))
    fig.suptitle('Trial-to-Trial Variability (single-threaded)', fontsize=14, fontweight='bold', y=1.02)

    components = [
        ('Total_ms',  'Total Runtime',  1000, 's'),
        ('Worker_ms', 'Worker Computation', 1000, 's'),
        ('Parse_ms',  'Parsing',        1,    'ms'),
    ]

    for ax, (col, title, divisor, unit) in zip(axes, components):
        data = [st[st['Algorithm']==a][col].values / divisor for a in algos]
        colors = [ALGO_COLORS.get(a, '#999') for a in algos]

        bp = ax.boxplot(data, tick_labels=algos, patch_artist=True, widths=0.5,
                        medianprops=dict(color='black', linewidth=1.5),
                        flierprops=dict(marker='o', markersize=4))
        for patch, c in zip(bp['boxes'], colors):
            patch.set_facecolor(c)
            patch.set_alpha(0.6)

        # Overlay individual points
        for i, (d, c) in enumerate(zip(data, colors)):
            jitter = np.random.normal(0, 0.04, size=len(d))
            ax.scatter(np.full_like(d, i+1) + jitter, d, color=c,
                       edgecolor='black', linewidth=0.4, s=30, zorder=5, alpha=0.8)

        ax.set_ylabel(f'Time ({unit})')
        ax.set_title(title)
        ax.grid(axis='y', alpha=0.3, linestyle='--')
        ax.spines['top'].set_visible(False)
        ax.spines['right'].set_visible(False)

    plt.tight_layout()
    plt.savefig(output_dir / 'variability_boxplots.png')
    plt.close()
    print(f"Saved: {output_dir / 'variability_boxplots.png'}")


# ── GRAPH 4: Thread scaling (if data exists) ──────────────────────────────

def plot_thread_scaling(df, output_dir):
    thread_counts = sorted(df['Threads'].unique())
    if len(thread_counts) < 2:
        return

    algos_with_threads = []
    for a in ALGO_ORDER:
        ad = df[df['Algorithm'] == a]
        if ad.empty:
            continue
        if ad['Threads'].nunique() >= 2:
            algos_with_threads.append(a)

    if not algos_with_threads:
        return

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))

    for algo in algos_with_threads:
        ad = df[df['Algorithm'] == algo]
        tc = sorted(ad['Threads'].unique())
        means = [ad[ad['Threads']==t]['Worker_ms'].mean()/1000 for t in tc]
        stds  = [ad[ad['Threads']==t]['Worker_ms'].std()/1000  for t in tc]
        c = ALGO_COLORS.get(algo, '#999')

        ax1.errorbar(tc, means, yerr=stds, marker='o', capsize=4, label=algo, color=c, linewidth=2)

        base = ad[ad['Threads']==1]['Worker_ms'].mean()
        speedups = [base / ad[ad['Threads']==t]['Worker_ms'].mean() for t in tc]
        ax2.plot(tc, speedups, marker='s', label=algo, color=c, linewidth=2)

    ax1.set_xlabel('Number of Threads')
    ax1.set_ylabel('Worker Time (seconds)')
    ax1.set_title('Worker Time vs Thread Count')
    ax1.legend()
    ax1.grid(alpha=0.3, linestyle='--')
    ax1.spines['top'].set_visible(False)
    ax1.spines['right'].set_visible(False)

    max_t = max(thread_counts)
    ax2.plot([1, max_t], [1, max_t], 'k--', alpha=0.4, label='Ideal')
    ax2.set_xlabel('Number of Threads')
    ax2.set_ylabel('Speedup (x)')
    ax2.set_title('Parallel Speedup')
    ax2.legend()
    ax2.grid(alpha=0.3, linestyle='--')
    ax2.spines['top'].set_visible(False)
    ax2.spines['right'].set_visible(False)

    plt.tight_layout()
    plt.savefig(output_dir / 'thread_scaling.png')
    plt.close()
    print(f"Saved: {output_dir / 'thread_scaling.png'}")


# ── Text report ────────────────────────────────────────────────────────────

def generate_report(df, output_dir):
    st = df[df['Threads'] == 1].copy()
    if st.empty:
        st = df.copy()

    f = output_dir / 'analysis_report.txt'
    with open(f, 'w') as out:
        W = 80
        out.write("=" * W + "\n")
        out.write("PASSWORD CRACKING PERFORMANCE ANALYSIS REPORT\n")
        out.write("=" * W + "\n\n")

        out.write("EXPERIMENT CONFIGURATION\n")
        out.write("-" * W + "\n")
        out.write("Character set:   79 printable ASCII characters (codes 33-111)\n")
        out.write("Password length: 3 characters\n")
        out.write("Search space:    79^3 = 493,039 candidates\n")
        algos = list(st['Algorithm'].unique())
        trials_per = st.groupby('Algorithm', observed=True).size()
        out.write(f"Algorithms:      {', '.join(str(a) for a in algos)}\n")
        out.write(f"Trials/algo:     {trials_per.iloc[0]}\n")
        thread_counts = sorted(df['Threads'].unique())
        if len(thread_counts) > 1:
            out.write(f"Thread counts:   {', '.join(map(str, thread_counts))}\n")
        out.write("\n")

        # Key findings
        out.write("=" * W + "\n")
        out.write("KEY FINDINGS\n")
        out.write("=" * W + "\n\n")

        total_means = st.groupby('Algorithm', observed=True)['Total_ms'].mean()
        slowest = total_means.idxmax()
        fastest = total_means.idxmin()
        ratio = total_means.max() / total_means.min()

        out.write(f"1. Fastest algorithm: {fastest} ({fmt_time(total_means[fastest])} average)\n")
        out.write(f"2. Slowest algorithm: {slowest} ({fmt_time(total_means[slowest])} average)\n")
        out.write(f"3. Performance ratio: {ratio:.1f}x (slowest / fastest)\n\n")

        out.write("4. Dominant component:\n")
        for algo in algos:
            ad = st[st['Algorithm'] == algo]
            wpct = ad['Worker_ms'].mean() / ad['Total_ms'].mean() * 100
            out.write(f"   {algo:<10} Worker computation = {wpct:.2f}% of total\n")

        out.write("\n5. Network + parsing overhead:\n")
        for algo in algos:
            ad = st[st['Algorithm'] == algo]
            oh = ad['Parse_ms'].mean() + ad['Dispatch_ms'].mean() + ad['Return_ms'].mean()
            out.write(f"   {algo:<10} {oh:.3f} ms ({oh / ad['Total_ms'].mean() * 100:.4f}% of total)\n")

        # Variability
        out.write(f"\n6. Variability (coefficient of variation on total time):\n")
        for algo in algos:
            ad = st[st['Algorithm'] == algo]
            cv = ad['Total_ms'].std() / ad['Total_ms'].mean() * 100
            out.write(f"   {algo:<10} CV = {cv:.3f}%\n")

        # Thread scaling
        if len(thread_counts) > 1:
            out.write(f"\n7. Thread scaling (MD5):\n")
            md5 = df[df['Algorithm'] == 'MD5']
            if not md5.empty:
                base = md5[md5['Threads']==1]['Worker_ms'].mean()
                for t in thread_counts:
                    td = md5[md5['Threads']==t]
                    if td.empty:
                        continue
                    wm = td['Worker_ms'].mean()
                    speedup = base / wm
                    eff = speedup / t * 100
                    out.write(f"   {t} thread(s): {fmt_time(wm)} worker, "
                              f"{speedup:.2f}x speedup, {eff:.1f}% efficiency\n")

        out.write("\n")
        out.write("=" * W + "\n")
        out.write("DETAILED STATISTICS (single-threaded)\n")
        out.write("=" * W + "\n\n")

        stats = st.groupby('Algorithm', observed=True)[
            ['Parse_ms','Dispatch_ms','Worker_ms','Return_ms','Total_ms']
        ].agg(['mean','std','min','max'])

        for algo in stats.index:
            out.write(f"{algo}:\n")
            out.write("-" * 70 + "\n")
            for col in ['Parse_ms','Dispatch_ms','Worker_ms','Return_ms','Total_ms']:
                m   = stats.loc[algo,(col,'mean')]
                s   = stats.loc[algo,(col,'std')]
                mn  = stats.loc[algo,(col,'min')]
                mx  = stats.loc[algo,(col,'max')]
                name = col.replace('_ms','').ljust(10)
                out.write(f"  {name}: {m:12.3f} +/- {s:8.3f} ms  [{mn:10.3f} - {mx:10.3f}]\n")
            out.write("\n")

        out.write("=" * W + "\n")
        out.write("METHODOLOGY\n")
        out.write("=" * W + "\n\n")
        out.write("- All times measured with CLOCK_MONOTONIC (high-resolution monotonic clock)\n")
        out.write("- Parse:    time to read and parse the shadow file on the controller\n")
        out.write("- Dispatch: network latency to send the cracking job to the worker\n")
        out.write("- Worker:   actual brute-force computation time on the worker process\n")
        out.write("- Return:   network latency to send the result back to the controller\n")
        out.write("- Total:    wall-clock time from controller start to result received\n\n")
        out.write("- Controller and worker communicate over TCP (localhost for these tests)\n")
        out.write("- Worker uses POSIX crypt_r() for thread-safe hash computation\n")
        out.write("- Each thread processes a strided subset of the candidate space\n")

    print(f"Saved: {f}")


# ── Main ───────────────────────────────────────────────────────────────────

def main():
    script_dir = Path(__file__).resolve().parent
    root_dir = script_dir.parent
    results_file = root_dir / 'results' / 'experiment_results.csv'
    output_dir = root_dir / 'results'
    output_dir.mkdir(exist_ok=True)

    print("\n" + "="*60)
    print("  PASSWORD CRACKING PERFORMANCE ANALYSIS")
    print("="*60)

    df = load_results(results_file)
    print(f"\nLoaded {len(df)} data points: "
          f"{df['Algorithm'].nunique()} algorithms, "
          f"thread counts: {sorted(df['Threads'].unique())}")

    # Tables
    print("\n")
    print_runtime_table(df, output_dir)
    print_component_table(df, output_dir)
    print_thread_table(df, output_dir)

    # Graphs
    print("\n" + "="*60)
    print("  GENERATING GRAPHS")
    print("="*60 + "\n")

    plot_total_runtime(df, output_dir)
    plot_component_breakdown(df, output_dir)
    plot_variability(df, output_dir)
    plot_thread_scaling(df, output_dir)

    # Report
    print()
    generate_report(df, output_dir)

    print("\n" + "="*60)
    print("  DONE")
    print("="*60)
    print("\nOutput files in results/:")
    for p in sorted(output_dir.glob('*.png')) + sorted(output_dir.glob('*.txt')):
        print(f"  {p.name}")
    print()


if __name__ == '__main__':
    main()
