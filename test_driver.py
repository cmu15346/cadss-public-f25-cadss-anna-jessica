import math
import subprocess
import csv
import time
import re

TRACE_PATH = "/afs/cs.cmu.edu/academic/class/15346-f23/public/traces/cache/astar.trace"
CADSS_CMD = "./cadss-engine"
CONFIG_FILE = "test_gen.config"
RESULTS_FILE = "test_results_astar.csv"

BUDGET_BITS = 54 * 8000  # 432000 bits

def run_sim(config_args):
    """Write config file, run cadss, capture ticks."""
    # Write test_gen.config
    with open(CONFIG_FILE, "w") as f:
        f.write("__processor\n")
        f.write(f"__cache {config_args}\n")
        f.write("__branch\n")
        f.write("__memory\n")
        f.write("__interconnect\n")
        f.write("__coherence\n\n")

    # Run cadss
    result = subprocess.run(
        [CADSS_CMD, "-s", CONFIG_FILE, "-c", "refCache", "-t", TRACE_PATH],
        capture_output=True,
        text=True,
        timeout=30  # avoid hanging forever
    )

    # Parse Ticks - #
    match = re.search(r"Ticks\s*-\s*(\d+)", result.stdout)
    ticks = int(match.group(1)) if match else None
    return ticks

def generate_random_configs():
    """Generate configs for Random replacement (-R)."""
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
                configs.append((E, b, i, k, total_bits))
                if 2 ** k > E:
                    break
                k += 1
    return configs

def generate_lru_configs():
    """Generate configs for LRU."""
    configs = []
    # for i in range(0, 1):
    #     for b in range(4, 11):
    #         victim_bits = 0
    #         if i > 0:
    #             victim_bits = i * (66 + math.log2(i) - b + (2 ** b))
    #         C = BUDGET_BITS - victim_bits
    #         if C <= 0:
    #             continue
    #         e = 1
    #         best_e = 0
    #         while True:
    #             line_bits = 66 + e - b + (2 ** b)
    #             size_bits = (2 ** e) * line_bits
    #             if size_bits > C:
    #                 break
    #             best_e = e
    #             e += 1
    #         if best_e > 0:
    #             E = 2 ** best_e
    #             main_bits = E * (66 + best_e - b + (2 ** b))
    #             total_bits = victim_bits + main_bits
    #             configs.append((E, b, i, None, total_bits))
    BUDGET_BITS = 54 * 8000  # 54KB = 432000 bits

    for i in range(0, 1):  # victim cache size i: 0 -> 8
        for b in range(4, 11):  # b = 4..10
            victim_bits = 0
            if i > 0:
                victim_bits = i * (66 + math.log2(i) - b + (2 ** b))

            C = BUDGET_BITS - victim_bits

            best_E = 0
            best_total_bits = 0

            # try increasing E until over budget
            for E in range(1, 100000):  # generous upper bound
                line_bits = 66 + math.ceil(math.log2(E)) - b + (2 ** b)
                size_bits = E * line_bits

                if size_bits > C:
                    break  # too big, stop searching

                best_E = E
                best_total_bits = victim_bits + size_bits

            if best_E > 0:
                # total_bytes = best_total_bits / 8
                configs.append((E, b, i, None, best_total_bits))
    return configs

def main():
    # open CSV once for append
    with open(RESULTS_FILE, "w", newline="") as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(["policy", "E", "b", "i", "k", "total_bits", "ticks"])

        print("generating RRIP configs")
        # RRIP configs
        for E, b, i, k, total_bits in generate_random_configs():
            args = f"-E {E} -b {b} -s 0 -i {i} -R {k}"
            print(args)
            ticks = run_sim(args)
            writer.writerow(["RRIP", E, b, i, k, total_bits, ticks])
            csvfile.flush()

        print("generating LRU configs")
        # LRU configs
        for E, b, i, k, total_bits in generate_lru_configs():
            args = f"-E {E} -b {b} -s 0 -i {i}"
            print(args)
            ticks = run_sim(args)
            writer.writerow(["LRU", E, b, i, k, total_bits, ticks])
            csvfile.flush()

if __name__ == "__main__":
    main()
