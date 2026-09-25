"""Генерирует тестовые IBL-карты (чистый Python, без зависимостей) в ibl/:
    irradiance.dds  - кубмап R16G16B16A16_FLOAT, 16x16, 1 mip
    prefilter.dds   - кубмап R16G16B16A16_FLOAT, 64x64, 7 mip (roughness = mip / 6)
    brdf_lut.dds    - 2D R16G16_FLOAT, 64x64, 1 mip
DDS с DX10-заголовком - ровно то, что читает LoadDds в RenderingSystem.cpp.

Окружение - процедурное небо (горизонт -> зенит, тёмная земля, тёплое солнце в -m_sunDir).
Это заглушки для проверки пайплайна; для красивого результата подставьте свои карты из HDRI (cmgen, IBLBaker, ...).

Запуск из корня репозитория:  python tools/make_test_ibl.py
"""
import math
import os
import struct

PI = math.pi

# ---------------- окружение ----------------
SUN_DIR = (-0.15, 0.96, -0.22)   # направление НА солнце (m_sunDir = 0.15, -0.96, 0.22 - куда светит)
_l = math.sqrt(sum(c * c for c in SUN_DIR))
SUN_DIR = tuple(c / _l for c in SUN_DIR)


def env(d):
    """Радианс окружения по направлению d (единичный вектор, Y вверх)."""
    y = d[1]
    if y >= 0:
        t = y ** 0.5
        c = [0.55 + (0.20 - 0.55) * t, 0.65 + (0.38 - 0.65) * t, 0.80 + (0.75 - 0.80) * t]   # горизонт -> зенит
    else:
        t = min(1.0, -y * 2.0)
        c = [0.42 + (0.10 - 0.42) * t, 0.38 + (0.09 - 0.38) * t, 0.33 + (0.08 - 0.33) * t]   # земля
    s = max(0.0, sum(a * b for a, b in zip(d, SUN_DIR)))
    sun = 12.0 * math.exp((s - 1.0) / 0.0015)   # маленькое яркое солнце
    return [c[0] + sun * 1.0, c[1] + sun * 0.93, c[2] + sun * 0.8]


# ---------------- сэмплирование ----------------
def radical_inverse(i):
    bits = i
    bits = ((bits << 16) | (bits >> 16)) & 0xFFFFFFFF
    bits = ((bits & 0x55555555) << 1) | ((bits & 0xAAAAAAAA) >> 1)
    bits = ((bits & 0x33333333) << 2) | ((bits & 0xCCCCCCCC) >> 2)
    bits = ((bits & 0x0F0F0F0F) << 4) | ((bits & 0xF0F0F0F0) >> 4)
    bits = ((bits & 0x00FF00FF) << 8) | ((bits & 0xFF00FF00) >> 8)
    return bits * 2.3283064365386963e-10


def hammersley(i, n):
    return (i / n, radical_inverse(i))


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def normalize(v):
    l = math.sqrt(v[0] ** 2 + v[1] ** 2 + v[2] ** 2)
    return (v[0] / l, v[1] / l, v[2] / l)


def basis(n):
    up = (0.0, 1.0, 0.0) if abs(n[1]) < 0.999 else (1.0, 0.0, 0.0)
    t = normalize(cross(up, n))
    b = cross(n, t)
    return t, b


def to_world(local, n):
    t, b = basis(n)
    return normalize((t[0] * local[0] + b[0] * local[1] + n[0] * local[2],
                      t[1] * local[0] + b[1] * local[1] + n[1] * local[2],
                      t[2] * local[0] + b[2] * local[1] + n[2] * local[2]))


def importance_sample_ggx(xi, roughness):
    a = roughness * roughness
    phi = 2.0 * PI * xi[0]
    cos_t = math.sqrt((1.0 - xi[1]) / (1.0 + (a * a - 1.0) * xi[1]))
    sin_t = math.sqrt(max(0.0, 1.0 - cos_t * cos_t))
    return (math.cos(phi) * sin_t, math.sin(phi) * sin_t, cos_t)


# ---------------- грани кубмапа (порядок и ориентация как в D3D: +X -X +Y -Y +Z -Z) ----------------
def face_dir(face, u, v):
    """u, v в [-1, 1]; v растёт вниз по картинке."""
    d = [(1, -v, -u), (-1, -v, u), (u, 1, v), (u, -1, -v), (u, -v, 1), (-u, -v, -1)][face]
    return normalize(d)


def build_cube(size, pixel_fn):
    """Возвращает список float-RGBA для одной грани за другой (строки сверху вниз)."""
    out = []
    for face in range(6):
        for y in range(size):
            for x in range(size):
                d = face_dir(face, (x + 0.5) / size * 2 - 1, (y + 0.5) / size * 2 - 1)
                out.extend(pixel_fn(d))
                out.append(1.0)
    return out


def irradiance_pixel(n, samples=96):
    """Свёртка с косинусом: возвращает E / PI (то есть радианс для равномерного окружения = сам радианс)."""
    acc = [0.0, 0.0, 0.0]
    for i in range(samples):
        xi = hammersley(i, samples)
        r = math.sqrt(xi[1])                 # cosine-weighted
        phi = 2.0 * PI * xi[0]
        local = (r * math.cos(phi), r * math.sin(phi), math.sqrt(max(0.0, 1.0 - xi[1])))
        c = env(to_world(local, n))
        acc[0] += c[0]; acc[1] += c[1]; acc[2] += c[2]
    return [a / samples for a in acc]


def prefilter_pixel(n, roughness, samples=64):
    if roughness < 1e-3:
        return env(n)
    acc = [0.0, 0.0, 0.0]
    weight = 0.0
    for i in range(samples):
        xi = hammersley(i, samples)
        h = to_world(importance_sample_ggx(xi, roughness), n)
        ndh = sum(a * b for a, b in zip(n, h))
        l = (2 * ndh * h[0] - n[0], 2 * ndh * h[1] - n[1], 2 * ndh * h[2] - n[2])   # V = N
        ndl = sum(a * b for a, b in zip(n, l))
        if ndl > 0:
            c = env(normalize(l))
            acc[0] += c[0] * ndl; acc[1] += c[1] * ndl; acc[2] += c[2] * ndl
            weight += ndl
    return [a / max(weight, 1e-6) for a in acc]


# ---------------- BRDF LUT ----------------
def integrate_brdf(ndv, roughness, samples=128):
    v = (math.sqrt(1.0 - ndv * ndv), 0.0, ndv)
    a_sum = b_sum = 0.0
    k = roughness * roughness / 2.0          # k для IBL
    for i in range(samples):
        xi = hammersley(i, samples)
        h = importance_sample_ggx(xi, roughness)      # N = (0,0,1)
        vdh = v[0] * h[0] + v[1] * h[1] + v[2] * h[2]
        l = (2 * vdh * h[0] - v[0], 2 * vdh * h[1] - v[1], 2 * vdh * h[2] - v[2])
        ndl = max(l[2], 0.0)
        ndh = max(h[2], 0.0)
        vdh = max(vdh, 0.0)
        if ndl > 0:
            g = (ndv / (ndv * (1 - k) + k)) * (ndl / (ndl * (1 - k) + k))
            g_vis = g * vdh / max(ndh * ndv, 1e-6)
            fc = (1.0 - vdh) ** 5
            a_sum += (1.0 - fc) * g_vis
            b_sum += fc * g_vis
    return a_sum / samples, b_sum / samples


# ---------------- запись DDS (DX10-заголовок) ----------------
def write_dds(path, width, height, mips, dxgi_format, is_cube, subresources):
    """subresources: список bytes в порядке 'грань -> mip' (как ждёт D3D12)."""
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | (0x20000 if mips > 1 else 0)
    caps = 0x1000 | ((0x8 | 0x400000) if mips > 1 else 0)
    caps2 = (0x200 | 0xFC00) if is_cube else 0
    header = struct.pack('<4s 7I 11I 8I 5I', b'DDS ', 124, flags, height, width, 0, 0, mips,
                         *([0] * 11),
                         32, 0x4, 0x30315844, 0, 0, 0, 0, 0,      # pixel format: FOURCC 'DX10'
                         caps, caps2, 0, 0, 0)
    dx10 = struct.pack('<5I', dxgi_format, 3, 0x4 if is_cube else 0, 1, 0)   # 3 = TEXTURE2D, miscFlag 4 = cube
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(header + dx10)
        for s in subresources:
            f.write(s)


def half_bytes(values):
    values = [min(max(v, 0.0), 65000.0) for v in values]
    return struct.pack('<%de' % len(values), *values)


def cube_faces_bytes(rgba, size):
    per_face = size * size * 4
    return [half_bytes(rgba[f * per_face:(f + 1) * per_face]) for f in range(6)]


def main():
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ibl')

    # irradiance: 16x16, 1 mip
    size = 16
    rgba = build_cube(size, irradiance_pixel)
    write_dds(os.path.join(out, 'irradiance.dds'), size, size, 1, 10, True, cube_faces_bytes(rgba, size))
    print('irradiance.dds done')

    # prefilter: 64x64, полная цепочка до 1x1 -> 7 уровней; roughness = mip / (mips - 1)
    base = 64
    mips = int(math.log2(base)) + 1
    per_mip = []
    for m in range(mips):
        s = base >> m
        rough = m / (mips - 1)
        per_mip.append(cube_faces_bytes(build_cube(s, lambda d, r=rough: prefilter_pixel(d, r)), s))
        print('prefilter mip', m, 'roughness', round(rough, 2))
    # порядок subresource: для каждой грани все mip подряд
    subs = [per_mip[m][face] for face in range(6) for m in range(mips)]
    write_dds(os.path.join(out, 'prefilter.dds'), base, base, mips, 10, True, subs)

    # BRDF LUT: X = NdotV, Y = roughness (строка 0 = roughness ~0), формат RG16F
    n = 64
    vals = []
    for y in range(n):
        rough = max((y + 0.5) / n, 0.02)
        for x in range(n):
            ndv = max((x + 0.5) / n, 0.02)
            a, b = integrate_brdf(ndv, rough)
            vals.extend((a, b))
    write_dds(os.path.join(out, 'brdf_lut.dds'), n, n, 1, 34, False, [half_bytes(vals)])
    print('brdf_lut.dds done')


if __name__ == '__main__':
    main()
