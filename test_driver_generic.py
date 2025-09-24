#!/usr/bin/env python3
import argparse
import math
import os
import re
import csv
import subprocess

CADSS_CMD = "./cadss-engine"
BUDGET_BITS = 54 * 8000  # 432000 bits (54 kB)
BASE_TRACE_DIR = "/afs/cs.cmu.edu/academic/class/15346-f23/public/traces/cache"

def build_paths(trace_name: str):
    # Allow either a bare name like "astar.trace" or a full path
    if os.path.sep in trace_name:
        trace_path = trace_name
        stem = os.path.splitext(os.path.basename(trace_name))[0]
    else:
        trace_path = os.path.join(BASE_TRACE_DIR, trace_name)
        stem = os.path.splitext(trace_name)[0]

    config_file = f"test_gen_{stem}.config"
    results_file = f"test_results_{stem}.csv"
    return trace_path, config_file, results_file, stem

def run_sim(config_args: str, trace_path: str, config_file: str, timeout_sec: int = 30):
    """
    Write the config file, run cadss, and return parsed ticks (int or None).
    """
    # Write uniquely-named config
    with open(config_file, "w") as f:
        f.write("__processor\n")
        f.write(f"__cache {config_args}\n")
        f.write("__branch\n")
        f.write("__memory\n")
        f.write("__interconnect\n")
        f.write("__coherence\n\n")

    # Run cadss
    try:
        result = subprocess.run(
            [CADSS_CMD, "-s", config_file, "-c", "refCache", "-t", trace_path],
            capture_output=True,
            text=True,
            timeout=timeout_sec
        )
    except subprocess.TimeoutExpired:
        print(f"[timeout] {CADSS_CMD} for {config_args}")
        return None

    combined_output = (result.stdout or "") + "\n" + (result.stderr or "")
    m = re.search(r"Ticks\s*-\s*(\d+)", combined_output)
    ticks = int(m.group(1)) if m else None

    if result.returncode != 0:
        print(f"[warn] cadss exited with {result.returncode}")
    if ticks is None:
        print("[warn] could not parse Ticks from output")
    return ticks

def generate_random_configs():
    """
    Generate configs for Random replacement (-R k).
    Uses closed-form E = floor(C / line_bits), with line_bits = 66 + k - b + 2^b.
    Only victim cache size i=0 here (matches your current loops).
    """
    configs = []
    for i in range(0, 1):
        for b in range(4, 11):
            victim_bits = 0
            if i > 0:
                victim_bits = i * (66 + math.log2(i) - b + (2 ** b))
            C = BUDGET_BITS - victim_bits
            if C <= 0:
                continue

            k = 1
            while True:
                line_bits = 66 + k - b + (2 ** b)
                if line_bits <= 0:
                    break
                E = int(C // line_bits)
                if E <= 0:
                    break
                main_bits = line_bits * E
                total_bits = victim_bits + main_bits
                configs.append(("RRIP", E, b, i, k, total_bits))
                # stop when the RRPV bits exceed #ways
                if 2 ** k > E:
                    break
                k += 1
    return configs

def generate_lru_configs():
    """
    Generate configs for LRU (no -R).
    We search for e = #bits in E (e = ceil(log2(E))) directly:
      find the largest e s.t. 2^e * (66 + e - b + 2^b) <= C
      then E = 2^e
    """
    configs = []
    for i in range(0, 1):
        for b in range(4, 11):
            victim_bits = 0
            if i > 0:
                victim_bits = i * (66 + math.log2(i) - b + (2 ** b))
            C = BUDGET_BITS - victim_bits
            if C <= 0:
                continue

            e = 1
            best_e = 0
            while True:
                line_bits = 66 + e - b + (2 ** b)
                size_bits = (2 ** e) * line_bits
                if size_bits > C:
                    break
                best_e = e
                e += 1

            if best_e > 0:
                E = 2 ** best_e
                main_bits = E * (66 + best_e - b + (2 ** b))
                total_bits = victim_bits + main_bits
                configs.append(("LRU", E, b, i, None, total_bits))
    return configs

def main():
    ap = argparse.ArgumentParser(description="Run CADSS cache sims over generated configs for a given trace.")
    ap.add_argument("trace_name", help="Trace file name (e.g., astar.trace) or full path")
    ap.add_argument("--timeout", type=int, default=30, help="Per-run timeout in seconds (default: 30)")
    args = ap.parse_args()

    trace_path, config_file, results_file, stem = build_paths(args.trace_name)
    if not os.path.exists(trace_path):
        raise FileNotFoundError(f"Trace not found: {trace_path}")

    print(f"Trace: {trace_path}")
    print(f"Config file: {config_file}")
    print(f"Results CSV: {results_file}")

    # Prepare CSV
    with open(results_file, "w", newline="") as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(["policy", "E", "b", "i", "k", "total_bits", "ticks"])

        print("Generating RRIP configs…")
        for policy, E, b, i, k, total_bits in generate_random_configs():
            args_str = f"-E {E} -b {b} -s 0 -i {i} -R {k}"
            print(args_str)
            ticks = run_sim(args_str, trace_path, config_file, timeout_sec=args.timeout)
            writer.writerow([policy, E, b, i, k, total_bits, ticks])
            csvfile.flush()

        print("Generating LRU configs…")
        for policy, E, b, i, k, total_bits in generate_lru_configs():
            args_str = f"-E {E} -b {b} -s 0 -i {i}"
            print(args_str)
            ticks = run_sim(args_str, trace_path, config_file, timeout_sec=args.timeout)
            writer.writerow([policy, E, b, i, k, total_bits, ticks])
            csvfile.flush()

    print(f"Done. Wrote results to {results_file}")

if __name__ == "__main__":
    main()
