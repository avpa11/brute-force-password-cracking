import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import sys
from pathlib import Path

def load_results(csv_file):
    try:
        df = pd.read_csv(csv_file)
        return df
    except FileNotFoundError:
        print(f"Error: Results file '{csv_file}' not found!")
        print("Please run ./run_experiments.sh first to generate data.")
        sys.exit(1)

def generate_summary_statistics(df):
    print("\n" + "="*80)
    print("SUMMARY STATISTICS (all times in milliseconds)")
    print("="*80)

    stats = df.groupby('Algorithm')[['Parse_ms', 'Dispatch_ms', 'Worker_ms',
                                       'Return_ms', 'Total_ms']].agg(['mean', 'std', 'min', 'max'])


    for algo in stats.index:
        print(f"\n{algo}:")
        print("-" * 70)
        for component in ['Parse_ms', 'Dispatch_ms', 'Worker_ms', 'Return_ms', 'Total_ms']:
            mean = stats.loc[algo, (component, 'mean')]
            std = stats.loc[algo, (component, 'std')]
            min_val = stats.loc[algo, (component, 'min')]
            max_val = stats.loc[algo, (component, 'max')]
            comp_name = component.replace('_ms', '').ljust(10)
            print(f"  {comp_name}: {mean:10.3f} ± {std:7.3f} ms  "
                  f"[{min_val:8.3f} - {max_val:8.3f}]")

    return stats

def create_comparison_table(stats):
    """Create a formatted comparison table"""
    print("\n" + "="*80)
    print("ALGORITHM COMPARISON TABLE")
    print("="*80)
    print(f"{'Algorithm':<12} {'Parse':<12} {'Dispatch':<12} {'Worker':<12} "
          f"{'Return':<12} {'Total':<12}")
    print(f"{'':12} {'(ms)':<12} {'(ms)':<12} {'(ms)':<12} {'(ms)':<12} {'(ms)':<12}")
    print("-" * 80)

    for algo in stats.index:
        parse_mean = stats.loc[algo, ('Parse_ms', 'mean')]
        dispatch_mean = stats.loc[algo, ('Dispatch_ms', 'mean')]
        worker_mean = stats.loc[algo, ('Worker_ms', 'mean')]
        return_mean = stats.loc[algo, ('Return_ms', 'mean')]
        total_mean = stats.loc[algo, ('Total_ms', 'mean')]

        print(f"{algo:<12} {parse_mean:>10.2f}  {dispatch_mean:>10.2f}  "
              f"{worker_mean:>10.2f}  {return_mean:>10.2f}  {total_mean:>10.2f}")

def analyze_components(df):
    """Analyze which components dominate the runtime"""
    print("\n" + "="*80)
    print("COMPONENT ANALYSIS")
    print("="*80)

    for algo in df['Algorithm'].unique():
        algo_data = df[df['Algorithm'] == algo]

        parse_pct = (algo_data['Parse_ms'].mean() / algo_data['Total_ms'].mean()) * 100
        dispatch_pct = (algo_data['Dispatch_ms'].mean() / algo_data['Total_ms'].mean()) * 100
        worker_pct = (algo_data['Worker_ms'].mean() / algo_data['Total_ms'].mean()) * 100
        return_pct = (algo_data['Return_ms'].mean() / algo_data['Total_ms'].mean()) * 100

        print(f"\n{algo}:")
        print(f"  Parse:        {parse_pct:6.2f}%")
        print(f"  Dispatch:     {dispatch_pct:6.2f}%")
        print(f"  Worker:       {worker_pct:6.2f}%  ← DOMINANT" if worker_pct > 50 else f"  Worker:       {worker_pct:6.2f}%")
        print(f"  Return:       {return_pct:6.2f}%")
        print(f"  Total:       100.00%")

def plot_total_runtime(df, output_dir):
    """Create bar chart of total runtime by algorithm"""
    fig, ax = plt.subplots(figsize=(10, 6))

    algorithms = df['Algorithm'].unique()
    means = [df[df['Algorithm'] == algo]['Total_ms'].mean() for algo in algorithms]
    stds = [df[df['Algorithm'] == algo]['Total_ms'].std() for algo in algorithms]

    bars = ax.bar(algorithms, means, yerr=stds, capsize=5, alpha=0.7, color='steelblue')

    ax.set_xlabel('Hash Algorithm', fontsize=12)
    ax.set_ylabel('Total Runtime (ms)', fontsize=12)
    ax.set_title('Total End-to-End Runtime by Algorithm', fontsize=14, fontweight='bold')
    ax.grid(axis='y', alpha=0.3)

    # Add value labels on bars
    for bar, mean in zip(bars, means):
        height = bar.get_height()
        ax.text(bar.get_x() + bar.get_width()/2., height,
                f'{mean:.1f}ms',
                ha='center', va='bottom', fontsize=10)

    plt.tight_layout()
    plt.savefig(output_dir / 'total_runtime.png', dpi=300)
    print(f"\nSaved: {output_dir / 'total_runtime.png'}")

def plot_component_breakdown(df, output_dir):
    """Create stacked bar chart showing component breakdown"""
    fig, ax = plt.subplots(figsize=(12, 7))

    algorithms = df['Algorithm'].unique()
    parse_means = [df[df['Algorithm'] == algo]['Parse_ms'].mean() for algo in algorithms]
    dispatch_means = [df[df['Algorithm'] == algo]['Dispatch_ms'].mean() for algo in algorithms]
    worker_means = [df[df['Algorithm'] == algo]['Worker_ms'].mean() for algo in algorithms]
    return_means = [df[df['Algorithm'] == algo]['Return_ms'].mean() for algo in algorithms]

    x = np.arange(len(algorithms))
    width = 0.6

    p1 = ax.bar(x, parse_means, width, label='Parse', color='#1f77b4')
    p2 = ax.bar(x, dispatch_means, width, bottom=parse_means, label='Dispatch', color='#ff7f0e')
    p3 = ax.bar(x, worker_means, width,
                bottom=[i+j for i,j in zip(parse_means, dispatch_means)],
                label='Worker Computation', color='#2ca02c')
    p4 = ax.bar(x, return_means, width,
                bottom=[i+j+k for i,j,k in zip(parse_means, dispatch_means, worker_means)],
                label='Return', color='#d62728')

    ax.set_xlabel('Hash Algorithm', fontsize=12)
    ax.set_ylabel('Time (ms)', fontsize=12)
    ax.set_title('Runtime Breakdown by Component', fontsize=14, fontweight='bold')
    ax.set_xticks(x)
    ax.set_xticklabels(algorithms)
    ax.legend(loc='upper left')
    ax.grid(axis='y', alpha=0.3)

    plt.tight_layout()
    plt.savefig(output_dir / 'component_breakdown.png', dpi=300)
    print(f"Saved: {output_dir / 'component_breakdown.png'}")

def plot_variability(df, output_dir):
    """Create box plots showing trial-to-trial variability"""
    fig, axes = plt.subplots(2, 3, figsize=(15, 10))
    fig.suptitle('Trial-to-Trial Variability Analysis', fontsize=16, fontweight='bold')

    components = ['Parse_ms', 'Dispatch_ms', 'Worker_ms', 'Return_ms', 'Total_ms']
    component_names = ['Parse', 'Dispatch', 'Worker', 'Return', 'Total']

    for idx, (component, name) in enumerate(zip(components, component_names)):
        row = idx // 3
        col = idx % 3
        ax = axes[row, col]

        data_to_plot = [df[df['Algorithm'] == algo][component].values
                        for algo in df['Algorithm'].unique()]

        bp = ax.boxplot(data_to_plot, labels=df['Algorithm'].unique(), patch_artist=True)

        # Color the boxes
        for patch in bp['boxes']:
            patch.set_facecolor('lightblue')

        ax.set_ylabel('Time (ms)', fontsize=10)
        ax.set_title(f'{name} Time', fontsize=12)
        ax.grid(axis='y', alpha=0.3)
        ax.tick_params(axis='x', rotation=15)

    # Hide the last subplot (we only have 5 plots)
    axes[1, 2].axis('off')

    plt.tight_layout()
    plt.savefig(output_dir / 'variability_boxplots.png', dpi=300)
    print(f"Saved: {output_dir / 'variability_boxplots.png'}")

def plot_worker_focus(df, output_dir):
    """Create detailed plot focusing on worker computation time"""
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6))

    algorithms = df['Algorithm'].unique()

    # Plot 1: Worker time with error bars
    means = [df[df['Algorithm'] == algo]['Worker_ms'].mean() for algo in algorithms]
    stds = [df[df['Algorithm'] == algo]['Worker_ms'].std() for algo in algorithms]

    bars = ax1.bar(algorithms, means, yerr=stds, capsize=5, alpha=0.7, color='green')
    ax1.set_xlabel('Hash Algorithm', fontsize=12)
    ax1.set_ylabel('Worker Computation Time (ms)', fontsize=12)
    ax1.set_title('Worker Computation Time by Algorithm', fontsize=13, fontweight='bold')
    ax1.grid(axis='y', alpha=0.3)

    for bar, mean in zip(bars, means):
        height = bar.get_height()
        ax1.text(bar.get_x() + bar.get_width()/2., height,
                f'{mean:.1f}ms',
                ha='center', va='bottom', fontsize=9)

    # Plot 2: Percentage of total time spent in worker
    percentages = [(df[df['Algorithm'] == algo]['Worker_ms'].mean() /
                    df[df['Algorithm'] == algo]['Total_ms'].mean() * 100)
                   for algo in algorithms]

    bars2 = ax2.bar(algorithms, percentages, alpha=0.7, color='orange')
    ax2.set_xlabel('Hash Algorithm', fontsize=12)
    ax2.set_ylabel('Worker Time / Total Time (%)', fontsize=12)
    ax2.set_title('Worker Computation as % of Total Runtime', fontsize=13, fontweight='bold')
    ax2.grid(axis='y', alpha=0.3)
    ax2.set_ylim([0, 100])

    for bar, pct in zip(bars2, percentages):
        height = bar.get_height()
        ax2.text(bar.get_x() + bar.get_width()/2., height,
                f'{pct:.1f}%',
                ha='center', va='bottom', fontsize=9)

    plt.tight_layout()
    plt.savefig(output_dir / 'worker_analysis.png', dpi=300)
    print(f"Saved: {output_dir / 'worker_analysis.png'}")

def generate_report(df, output_dir):
    """Generate a comprehensive text report"""
    report_file = output_dir / 'analysis_report.txt'

    with open(report_file, 'w') as f:
        f.write("="*80 + "\n")
        f.write("PASSWORD CRACKING PERFORMANCE ANALYSIS REPORT\n")
        f.write("="*80 + "\n\n")

        f.write("EXPERIMENT CONFIGURATION\n")
        f.write("-" * 80 + "\n")
        f.write(f"Character Set: 79 characters (ASCII 33-111)\n")
        f.write(f"Password Length: 3 characters\n")
        f.write(f"Search Space: 79³ = 493,039 candidates\n")
        f.write(f"Number of Trials: {df.groupby('Algorithm').size().iloc[0]}\n")
        f.write(f"Algorithms Tested: {', '.join(df['Algorithm'].unique())}\n\n")

        f.write("="*80 + "\n")
        f.write("KEY FINDINGS\n")
        f.write("="*80 + "\n\n")

        # Find slowest and fastest algorithms
        total_means = df.groupby('Algorithm')['Total_ms'].mean()
        slowest = total_means.idxmax()
        fastest = total_means.idxmin()

        f.write(f"1. Slowest Algorithm: {slowest} ({total_means[slowest]:.2f} ms average)\n")
        f.write(f"2. Fastest Algorithm: {fastest} ({total_means[fastest]:.2f} ms average)\n")
        f.write(f"3. Performance Range: {total_means.max() / total_means.min():.2f}x difference\n\n")

        # Dominant component analysis
        f.write("4. Dominant Cost Component:\n")
        for algo in df['Algorithm'].unique():
            algo_data = df[df['Algorithm'] == algo]
            worker_pct = (algo_data['Worker_ms'].mean() / algo_data['Total_ms'].mean()) * 100
            f.write(f"   - {algo}: Worker computation ({worker_pct:.1f}% of total time)\n")

        f.write("\n")
        f.write("5. Distributed Overhead:\n")
        for algo in df['Algorithm'].unique():
            algo_data = df[df['Algorithm'] == algo]
            overhead_pct = ((algo_data['Parse_ms'].mean() +
                           algo_data['Dispatch_ms'].mean() +
                           algo_data['Return_ms'].mean()) /
                          algo_data['Total_ms'].mean()) * 100
            f.write(f"   - {algo}: {overhead_pct:.1f}% of total time is distributed overhead\n")

        f.write("\n")
        f.write("="*80 + "\n")
        f.write("DETAILED STATISTICS\n")
        f.write("="*80 + "\n\n")

        stats = df.groupby('Algorithm')[['Parse_ms', 'Dispatch_ms', 'Worker_ms',
                                          'Return_ms', 'Total_ms']].agg(['mean', 'std', 'min', 'max'])

        for algo in stats.index:
            f.write(f"{algo}:\n")
            f.write("-" * 70 + "\n")
            for component in ['Parse_ms', 'Dispatch_ms', 'Worker_ms', 'Return_ms', 'Total_ms']:
                mean = stats.loc[algo, (component, 'mean')]
                std = stats.loc[algo, (component, 'std')]
                min_val = stats.loc[algo, (component, 'min')]
                max_val = stats.loc[algo, (component, 'max')]
                comp_name = component.replace('_ms', '').ljust(10)
                f.write(f"  {comp_name}: {mean:10.3f} ± {std:7.3f} ms  "
                       f"[{min_val:8.3f} - {max_val:8.3f}]\n")
            f.write("\n")

        f.write("="*80 + "\n")
        f.write("METHODOLOGY AND OBSERVATIONS\n")
        f.write("="*80 + "\n\n")

        f.write("Measurement Methodology:\n")
        f.write("- All times measured using CLOCK_MONOTONIC for accuracy\n")
        f.write("- Parse time: Controller shadow file parsing\n")
        f.write("- Dispatch time: Network latency sending job to worker\n")
        f.write("- Worker time: Actual password cracking computation on worker\n")
        f.write("- Return time: Network latency receiving result from controller\n")
        f.write("- Total time: End-to-end runtime including all components\n\n")

        f.write("Variability Observations:\n")
        for algo in df['Algorithm'].unique():
            algo_data = df[df['Algorithm'] == algo]
            cv = (algo_data['Total_ms'].std() / algo_data['Total_ms'].mean()) * 100
            f.write(f"- {algo}: Coefficient of variation = {cv:.2f}%\n")

        f.write("\n")
        f.write("Distributed System Observations:\n")
        f.write("- The worker computation dominates total runtime for all algorithms\n")
        f.write("- Network and parsing overhead is minimal compared to computation\n")
        f.write("- This design highlights the limitation of distributed execution\n")
        f.write("  when no computational parallelism is available\n")
        f.write("- Single-threaded worker becomes the bottleneck\n")

    print(f"\nSaved: {report_file}")

def main():
    """Main analysis function"""
    # Configuration
    results_file = Path('results/experiment_results.csv')
    output_dir = Path('results')

    print("\n" + "="*80)
    print("  PASSWORD CRACKING PERFORMANCE ANALYSIS")
    print("="*80)

    # Load data
    print(f"\nLoading results from: {results_file}")
    df = load_results(results_file)
    print(f"Loaded {len(df)} data points from {df['Algorithm'].nunique()} algorithms")

    # Generate statistics
    stats = generate_summary_statistics(df)
    create_comparison_table(stats)
    analyze_components(df)

    # Generate plots
    print("\n" + "="*80)
    print("GENERATING VISUALIZATIONS")
    print("="*80)

    plot_total_runtime(df, output_dir)
    plot_component_breakdown(df, output_dir)
    plot_variability(df, output_dir)
    plot_worker_focus(df, output_dir)

    # Generate report
    print("\n" + "="*80)
    print("GENERATING REPORT")
    print("="*80)
    generate_report(df, output_dir)

    print("\n" + "="*80)
    print("  ANALYSIS COMPLETE")
    print("="*80)
    print("\nGenerated files:")
    print(f"  - {output_dir / 'total_runtime.png'}")
    print(f"  - {output_dir / 'component_breakdown.png'}")
    print(f"  - {output_dir / 'variability_boxplots.png'}")
    print(f"  - {output_dir / 'worker_analysis.png'}")
    print(f"  - {output_dir / 'analysis_report.txt'}")
    print()

if __name__ == '__main__':
    main()
