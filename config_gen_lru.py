import math

def generate_configs_lru():
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
                total_bytes = best_total_bits / 8
                print(
                    f"__cache -E {best_E} -b {b} -s 0 -i {i} "
                    f"# total={best_total_bits:.0f} bits ({total_bytes:.1f} bytes)"
                )

if __name__ == "__main__":
    generate_configs_lru()
