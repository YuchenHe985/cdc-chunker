"""Reference implementation of the two chunkers, written from their definitions.

It shares no code with the C++ library and does not use rolling updates: every cut decision is
computed from scratch from the last 64 (Gear) or 48 (Rabin) bytes, so it is slow and only meant
for cross-checking the library on small inputs (see cross_check.py).
"""

MASK64 = (1 << 64) - 1
GEAR_SEED = 0x243F6A8885A308D3
GEAR_WINDOW = 64
RABIN_POLY = 0x3DA3358B4DC173
RABIN_WINDOW = 48


def splitmix64(state):
    state = (state + 0x9E3779B97F4A7C15) & MASK64
    z = state
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
    return state, z ^ (z >> 31)


def random_bytes(n, seed):
    out = bytearray()
    state = seed
    while len(out) < n:
        state, v = splitmix64(state)
        out += v.to_bytes(8, "little")
    return bytes(out[:n])


def gear_table():
    state, table = GEAR_SEED, []
    for _ in range(256):
        state, v = splitmix64(state)
        table.append(v)
    return table


def top_mask(bits):
    return 0 if bits <= 0 else (MASK64 << (64 - bits)) & MASK64


# ---- GF(2) polynomial arithmetic -------------------------------------------------------------

def poly_degree(a):
    return a.bit_length() - 1


def poly_mod(a, p):
    dp = poly_degree(p)
    while a.bit_length() > dp:
        a ^= p << (a.bit_length() - 1 - dp)
    return a


def poly_mulmod(a, b, p):
    r = 0
    while b:
        if b & 1:
            r ^= a
        b >>= 1
        a <<= 1
    return poly_mod(r, p)


def poly_gcd(a, b):
    while b:
        a, b = b, poly_mod(a, b)
    return a


def is_irreducible(p):
    """Rabin's test: x^(2^n) = x (mod p) and gcd(x^(2^(n/q)) - x, p) = 1 for every prime q dividing n."""
    n = poly_degree(p)

    def x_pow_2k(k):
        v = 2
        for _ in range(k):
            v = poly_mulmod(v, v, p)
        return v

    if x_pow_2k(n) != poly_mod(2, p):
        return False
    primes = [q for q in range(2, n + 1) if n % q == 0 and all(q % r for r in range(2, q))]
    return all(poly_gcd(p, x_pow_2k(n // q) ^ 2) == 1 for q in primes)


# ---- boundaries ------------------------------------------------------------------------------

def gear_boundaries(data, min_size, avg_size, max_size, norm):
    """Chunk lengths. The hash of a window is sum_j table[byte j positions back] << j (mod 2^64)."""
    g = gear_table()
    bits = avg_size.bit_length() - 1
    strict, relaxed = top_mask(bits + norm), top_mask(bits - norm)
    cuts, start, n = [], 0, len(data)
    while start < n:
        limit = min(max_size, n - start)
        cut = limit
        for length in range(min_size, limit + 1):
            i = start + length - 1
            fp = sum(g[data[i - j]] << j for j in range(GEAR_WINDOW)) & MASK64
            if fp & (strict if length < avg_size else relaxed) == 0:
                cut = length
                break
        cuts.append(cut)
        start += cut
    return cuts


def rabin_boundaries(data, min_size, avg_size, max_size):
    """The fingerprint of a window is its bytes read as a GF(2) polynomial, modulo RABIN_POLY."""
    # contribution of the byte at window position j: b * x^(8 * (window - 1 - j)) mod p
    power = [1]
    for _ in range(RABIN_WINDOW):
        power.append(poly_mod(power[-1] << 8, RABIN_POLY))
    table = [[poly_mulmod(b, power[RABIN_WINDOW - 1 - j], RABIN_POLY) for b in range(256)] for j in range(RABIN_WINDOW)]
    mask = avg_size - 1
    cuts, start, n = [], 0, len(data)
    while start < n:
        limit = min(max_size, n - start)
        cut = limit
        for length in range(min_size, limit + 1):
            first = start + length - RABIN_WINDOW
            digest = 0
            for j in range(RABIN_WINDOW):
                digest ^= table[j][data[first + j]]
            if digest & mask == 0:
                cut = length
                break
        cuts.append(cut)
        start += cut
    return cuts
