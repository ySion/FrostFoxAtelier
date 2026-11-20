"""Check Boolean intersection and half-space arithmetic with Python integers."""

from itertools import product


def require(condition, message):
    if not condition:
        raise ArithmeticError(message)


def signed(magnitude, bits):
    # Symmetric closed interval [-magnitude, magnitude], including either sign.
    require(0 <= magnitude < 1 << (bits - 1), f"{magnitude} exceeds int{bits}")
    return magnitude


def widths():
    n, d = (1 << 52) - 1, (1 << 81) - 1
    x, w = (1 << 190) - 1, (1 << 160) - 1

    product_nn = signed(n*n, 128)
    minor = signed(2*product_nn, 128)
    numerator_term = signed(d*minor, 256)
    denominator_term = signed(n*minor, 256)
    for terms in (1, 2, 3):
        numerator = signed(terms*numerator_term, 256)
        denominator = signed(terms*denominator_term, 256)
    # The symmetric interval also covers negating the four components.
    require(numerator <= x and denominator <= w, "intersection exceeds point budget")

    for terms in (1, 2, 3):
        linear = signed(terms*n*x, 256)
        native = signed(terms*n*(1 << 50), 128)
    evaluation = signed(linear + signed(d*w, 256), 256)
    native_evaluation = signed(native + d, 128)
    print("magnitude bits: minor, numerator, denominator, side, side(w=1) =",
          *(v.bit_length() for v in (minor, numerator, denominator, evaluation, native_evaluation)))


def intersection(a, b, c):
    p = [0, 0, 0, 0]
    for k in range(3):
        i, j = (k+1) % 3, (k+2) % 3
        bc = b[i]*c[j] - b[j]*c[i]
        ca = c[i]*a[j] - c[j]*a[i]
        ab = a[i]*b[j] - a[j]*b[i]
        p[k] = -(a[3]*bc + b[3]*ca + c[3]*ab)
        p[3] += a[k]*bc
    return p


def identities():
    # Each residual is multiaffine in the twelve coefficients. For coordinate k
    # its numerator omits column k, so multiplying by the row's coefficient k
    # cannot square that variable. The w term likewise omits column 3.
    # A multiaffine polynomial is determined by its values on the Boolean cube.
    for values in product((0, 1), repeat=12):
        rows = [values[i:i+4] for i in (0, 4, 8)]
        p = intersection(*rows)
        for row in rows:
            require(sum(u*v for u, v in zip(row, p)) == 0, "plane residual")
        a, b, c = rows
        # Independent six-permutation expansion of the normal determinant.
        det = (a[0]*b[1]*c[2] + a[1]*b[2]*c[0] + a[2]*b[0]*c[1]
               - a[2]*b[1]*c[0] - a[1]*b[0]*c[2] - a[0]*b[2]*c[1])
        require(det == p[3], "denominator orientation")
    print("4096 multiaffine coefficient vertices: three residuals and denominator verified")


def bounds_witness():
    m, k = 262149, 262153
    p = intersection((-m, 1, 0, m*k), (0, -m, 1, 0), (1, 0, m, -k-1))
    if p[3] < 0:
        p = [-v for v in p]
    n, w = p[0], p[3]
    require(w == m**3 + 1 and n == k*w + 1, "intersection witness")
    candidate = int(float(n) / float(w))  # Positive witness, truncation equals floor.
    require(candidate == k - 1, "expected binary64 downward rounding")
    require((candidate+1)*w <= n < (candidate+2)*w, "upward repair")
    print("binary64 floor witness: verified")


if __name__ == "__main__":
    widths()
    identities()
    bounds_witness()
