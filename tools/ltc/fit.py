"""Fit linearly transformed cosines to the Scene Lit specular BRDF.

Writes src/renderer/ltc_lut.hpp. The BRDF is lit_brdf_components with F = 1:
GGX D, separable Smith G1(v) G1(l), alpha = max(roughness^2, .0025). The fit
follows Heitz et al. 2016 (Nelder-Mead on the MIS cubed-error metric, warm
started from the neighbouring cell); only the BRDF differs from their GGX fit.

Rows: theta parameter x = sqrt(1 - cos theta_v); columns: roughness. Matrix
entries are the inverse transform normalised by its y scale; amplitude stores
the specular albedo with F = 1 and its Schlick (1 - v.h)^5 moment.
"""
import math
import pathlib
import sys
import time

import numpy as np

N = 64
ERROR_SAMPLES = 32
MOMENT_SAMPLES = 256
LOCK = pathlib.Path(r"E:\FrostFoxAtelier\.amp\in\locks\gpu-window.lock")


def wait_for_quiet_machine():
    # Another holder's measurement must not share the machine with this fit.
    while LOCK.exists():
        try:
            text = LOCK.read_text(encoding="utf-8", errors="replace")
        except OSError:
            text = ""
        if text.startswith("[builds-allowed]") or (text.startswith("pid=") and " acceptance-gate windows=" in text):
            return
        time.sleep(5)


def grid(n):
    u = (np.arange(n) + .5) / n
    u1, u2 = np.meshgrid(u, u, indexing="ij")
    return u1.ravel(), u2.ravel()


def brdf(v, l, alpha):
    """Specular BRDF * cos with F = 1, and the GGX D-sampling pdf of l."""
    nl = np.clip(l[:, 2], 0, 1)
    nv = max(min(v[2], 1.), 1e-4)
    h = v[None, :] + l
    hl = np.linalg.norm(h, axis=1)
    hl = np.where(hl > 0, hl, 1)
    h = h / hl[:, None]
    # The sampler reaches l through +-h, so its pdf uses |n.h| and |v.h|;
    # the value is zero below the horizon, where n.h > 0 holds otherwise.
    nh = np.minimum(np.abs(h[:, 2]), 1)
    vh = h @ v
    a2 = alpha * alpha
    d = a2 / (math.pi * (nh * nh * (a2 - 1) + 1) ** 2)
    gv = 2 * nv / (nv + math.sqrt(a2 + (1 - a2) * nv * nv))
    gl = 2 * nl / (nl + np.sqrt(a2 + (1 - a2) * nl * nl))
    value = d * gv * gl / np.maximum(4 * nl * nv, 1e-5) * nl
    pdf = d * nh / (4 * np.maximum(np.abs(vh), 1e-7))
    return value, pdf, np.clip(vh, 0, 1)


def sample_brdf(v, alpha, u1, u2):
    phi = 2 * math.pi * u2
    a2 = alpha * alpha
    cos_h = np.sqrt((1 - u1) / (1 + (a2 - 1) * u1))
    sin_h = np.sqrt(np.maximum(0, 1 - cos_h * cos_h))
    h = np.stack([sin_h * np.cos(phi), sin_h * np.sin(phi), cos_h], axis=1)
    return 2 * (h @ v)[:, None] * h - v[None, :]


class Ltc:
    def __init__(self):
        self.m11 = self.m22 = 1.
        self.m13 = 0.
        self.frame = np.eye(3)
        self.magnitude = 1.
        self.update()

    def update(self):
        local = np.array([[self.m11, 0, self.m13], [0, self.m22, 0], [0, 0, 1.]])
        self.m = self.frame @ local
        self.inverse = np.linalg.inv(self.m)
        self.det = abs(np.linalg.det(self.m))

    def evaluate(self, l):
        original = l @ self.inverse.T
        length = np.linalg.norm(original, axis=1)
        cosine = np.maximum(original[:, 2] / length, 0)
        pdf = cosine / math.pi / (self.det * length ** 3)
        return self.magnitude * pdf, pdf

    def sample(self, u1, u2):
        theta = np.arccos(np.sqrt(u1))
        phi = 2 * math.pi * u2
        local = np.stack([np.sin(theta) * np.cos(phi), np.sin(theta) * np.sin(phi), np.cos(theta)], axis=1)
        l = local @ self.m.T
        return l / np.linalg.norm(l, axis=1)[:, None]


ERROR_U1, ERROR_U2 = grid(ERROR_SAMPLES)
MOMENT_U1, MOMENT_U2 = grid(MOMENT_SAMPLES)


def error(ltc, v, alpha):
    total = 0.
    for l in (ltc.sample(ERROR_U1, ERROR_U2), sample_brdf(v, alpha, ERROR_U1, ERROR_U2)):
        target, target_pdf, _ = brdf(v, l, alpha)
        approximation, approximation_pdf = ltc.evaluate(l)
        difference = np.abs(target - approximation) ** 3
        total += np.sum(difference / np.maximum(approximation_pdf + target_pdf, 1e-30))
    return total / ERROR_U1.size


def moments(v, alpha):
    l = sample_brdf(v, alpha, MOMENT_U1, MOMENT_U2)
    value, pdf, vh = brdf(v, l, alpha)
    weight = np.where((pdf > 0) & (l[:, 2] > 0), value / np.maximum(pdf, 1e-30), 0)
    direction = np.sum(weight[:, None] * l, axis=0)
    direction[1] = 0
    direction /= np.linalg.norm(direction)
    return np.mean(weight), np.mean(weight * (1 - vh) ** 5), direction


def nelder_mead(function, start, delta, tolerance=1e-5, iterations=100):
    dimension = len(start)
    points = [np.array(start, dtype=float)]
    for i in range(dimension):
        point = np.array(start, dtype=float)
        point[i] += delta
        points.append(point)
    values = [function(p) for p in points]
    for _ in range(iterations):
        order = np.argsort(values, kind="stable")
        points = [points[i] for i in order]
        values = [values[i] for i in order]
        low, high = abs(values[0]), abs(values[-1])
        if 2 * abs(low - high) < (low + high) * tolerance:
            break
        centroid = np.mean(points[:-1], axis=0)
        reflected = centroid + (centroid - points[-1])
        reflected_value = function(reflected)
        if reflected_value < values[-2]:
            if reflected_value < values[0]:
                expanded = centroid + 2 * (centroid - points[-1])
                expanded_value = function(expanded)
                if expanded_value < reflected_value:
                    points[-1], values[-1] = expanded, expanded_value
                    continue
            points[-1], values[-1] = reflected, reflected_value
            continue
        contracted = centroid - .5 * (centroid - points[-1])
        contracted_value = function(contracted)
        if contracted_value < values[-1]:
            points[-1], values[-1] = contracted, contracted_value
            continue
        for i in range(1, len(points)):
            points[i] = points[0] + .5 * (points[i] - points[0])
            values[i] = function(points[i])
    best = int(np.argmin(values))
    return points[best]


def fit():
    matrices = np.zeros((N, N, 3, 3))
    amplitudes = np.zeros((N, N, 2))
    ltc = Ltc()
    for a in range(N - 1, -1, -1):
        wait_for_quiet_machine()
        roughness = a / (N - 1)
        alpha = max(roughness * roughness, .0025)
        for t in range(N):
            # The shader clamps n.v to 1e-4; the last row fits that endpoint.
            x = t / (N - 1)
            nv = max(1 - x * x, 1e-4)
            v = np.array([math.sqrt(1 - nv * nv), 0, nv])
            norm, fresnel, direction = moments(v, alpha)
            ltc.magnitude = norm
            isotropic = t == 0
            if isotropic:
                ltc.frame = np.eye(3)
                if a == N - 1:
                    ltc.m11 = ltc.m22 = 1.
                else:
                    ltc.m11, ltc.m22 = matrices[a + 1, 0, 0, 0], matrices[a + 1, 0, 1, 1]
                ltc.m13 = 0.
            else:
                ltc.frame = np.array([[direction[2], 0, direction[0]], [0, 1, 0], [-direction[0], 0, direction[2]]])
            ltc.update()

            def objective(parameters):
                m11 = max(parameters[0], 1e-7)
                ltc.m11 = m11
                ltc.m22 = m11 if isotropic else max(parameters[1], 1e-7)
                ltc.m13 = 0. if isotropic else parameters[2]
                ltc.update()
                return error(ltc, v, alpha)

            best = nelder_mead(objective, [ltc.m11, ltc.m22, ltc.m13], .05)
            objective(best)
            m = ltc.m.copy()
            m[0, 1] = m[1, 0] = m[1, 2] = m[2, 1] = 0
            matrices[a, t] = m
            amplitudes[a, t] = norm, fresnel
        print(f"roughness row {a} done", flush=True)
    return matrices, amplitudes


def write(matrices, amplitudes, path):
    entries = []
    for t in range(N):
        for a in range(N):
            inverse = np.linalg.inv(matrices[a, t])
            inverse /= inverse[1, 1]
            entries.append((inverse[0, 0], inverse[2, 0], inverse[0, 2], inverse[2, 2]))
    def number(value):
        text = f"{np.float32(value).item():.9g}"
        return text + ("f" if any(c in text for c in ".e") else ".f")
    lines = [
        "#pragma once",
        "// Generated by tools/ltc/fit.py; do not edit.",
        "#include <array>",
        "",
        "namespace atelier::renderer::ltc_lut {",
        f"inline constexpr unsigned size = {N};",
        "// Per cell (row theta parameter, column roughness): inverse transform",
        "// entries m00, m20, m02, m22 with m11 = 1, then specular albedo and",
        "// Schlick moment, so each cell is six floats.",
        f"inline constexpr std::array<float, {N * N * 6}> table{{",
    ]
    for t in range(N):
        for a in range(N):
            m00, m20, m02, m22 = entries[t * N + a]
            norm, fresnel = amplitudes[a, t]
            lines.append("    " + ",".join(number(value) for value in (m00, m20, m02, m22, norm, fresnel)) + ",")
    lines += ["};", "}", ""]
    path.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def main():
    root = pathlib.Path(__file__).resolve().parents[2]
    output = root / "src" / "renderer" / "ltc_lut.hpp"
    started = time.time()
    matrices, amplitudes = fit()
    np.savez(root / ".amp" / "in" / "ltc_fit.npz", matrices=matrices, amplitudes=amplitudes)
    write(matrices, amplitudes, output)
    print(f"wrote {output} in {time.time() - started:.0f} s")


if __name__ == "__main__":
    sys.exit(main())
