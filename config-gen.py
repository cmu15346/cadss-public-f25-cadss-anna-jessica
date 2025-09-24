import math

def generate_configs():
    BUDGET_BITS = 54 * 8000  # 54KB = 432000 bits

    for i in range(0, 1):  # victim cache size i: 0 -> 8
        for b in range(4, 11):  # b = 4,5,6,7,8,9,10
            victim_bits = 0
            if i > 0:
                victim_bits = i * (66 + math.log2(i) - b + (2 ** b))

            # Remaining budget for main cache
            C = BUDGET_BITS - victim_bits

            k = 1
            while True:
                line_bits = 66 + k - b + (2 ** b)
                if line_bits <= 0:
                    break  # nonsense case

                # closed form E
                E = int(C // line_bits)

                if E <= 0:
                    break

                # main cache size in bits
                main_bits = line_bits * E

                # total size in bits (victim + main)
                total_bits = victim_bits + main_bits
                total_bytes = total_bits / 8

                # output config + sizes
                print(
                    f"__cache -E {E} -b {b} -s 0 -i {i} -R {k} "
                    f"# total={total_bits:.0f} bits ({total_bytes:.1f} bytes)"
                )

                # stop if 2^k > E
                if 2 ** k > E:
                    break

                k += 1

if __name__ == "__main__":
    generate_configs()
