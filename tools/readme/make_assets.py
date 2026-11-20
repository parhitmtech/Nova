#!/usr/bin/env python3
"""Generate the README's SVG graphics from the numbers in docs/BENCHMARKS.md and
compiler/kernels/README.md. Every SVG has its own dark card background so it reads the same on
GitHub's light and dark themes. Animations are CSS inside the SVG (GitHub plays them in <img>),
and stop under prefers-reduced-motion."""
import math, os, sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "docs/assets"
os.makedirs(OUT, exist_ok=True)

BG, SURF, BORDER = "#0c101b", "#111725", "#242e43"
TEXT, MUTED = "#e6eaf2", "#99a6be"
ACCENT, TEAL, PINK, AMBER, SKY = "#a5a2ff", "#76dac4", "#f5a6b1", "#f2c879", "#7cb8f2"
SANS = "-apple-system, BlinkMacSystemFont, 'Segoe UI', Inter, Helvetica, Arial, sans-serif"
MONO = "ui-monospace, SFMono-Regular, Menlo, Consolas, 'DejaVu Sans Mono', monospace"

BASE_CSS = f"""
  text {{ font-family: {SANS}; fill: {TEXT}; }}
  .mono {{ font-family: {MONO}; }}
  .muted {{ fill: {MUTED}; }}
  .title {{ font-size: 20px; font-weight: 700; }}
  .sub {{ font-size: 13px; fill: {MUTED}; }}
  .lbl {{ font-size: 13px; }}
  .val {{ font-size: 12px; font-weight: 600; }}
  .tick {{ font-size: 11px; fill: {MUTED}; }}
  .grid {{ stroke: {BORDER}; stroke-width: 1; }}
  @media (prefers-reduced-motion: reduce) {{ * {{ animation: none !important; }} }}
"""


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def card(w, h, body, css=""):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}" '
            f'role="img">\n<style>{BASE_CSS}{css}</style>\n'
            f'<rect x="0.5" y="0.5" width="{w-1}" height="{h-1}" rx="14" fill="{BG}" stroke="{BORDER}"/>\n'
            f'{body}\n</svg>\n')


def write(name, svg):
    with open(os.path.join(OUT, name), "w") as f:
        f.write(svg)
    print("wrote", name)


# ── 1. Hero banner: wordmark, tagline, and source flowing through the compiler ────────────────
def hero():
    W, H = 1200, 360
    stages = ["source", "lexer", "parse tree", "check pass", "IR + passes", "interpreter", "CUDA"]
    css = f"""
  .word {{ font-size: 64px; font-weight: 800; letter-spacing: -1.5px; }}
  .tag {{ font-size: 19px; fill: {MUTED}; }}
  .stage {{ font-size: 13px; font-weight: 600; }}
  .pulse {{ animation: flow 4.2s linear infinite; }}
  @keyframes flow {{ 0% {{ offset-distance: 0%; opacity: 0; }} 6% {{ opacity: 1; }}
                     94% {{ opacity: 1; }} 100% {{ offset-distance: 100%; opacity: 0; }} }}
  .glow {{ animation: glow 4.2s ease-in-out infinite; }}
  @keyframes glow {{ 0%,100% {{ opacity: .35; }} 50% {{ opacity: .9; }} }}
  .caret {{ animation: blink 1.1s steps(1) infinite; }}
  @keyframes blink {{ 50% {{ opacity: 0; }} }}
  .typed {{ animation: type 3.2s steps(34) 0.4s both; }}
  @keyframes type {{ from {{ width: 0; }} to {{ width: 330px; }} }}
"""
    b = []
    # soft background blobs
    b.append(f'<defs><radialGradient id="g1" cx="0.15" cy="0.2" r="0.6"><stop offset="0" stop-color="{ACCENT}" stop-opacity=".22"/>'
             f'<stop offset="1" stop-color="{ACCENT}" stop-opacity="0"/></radialGradient>'
             f'<radialGradient id="g2" cx="0.9" cy="0.9" r="0.6"><stop offset="0" stop-color="{TEAL}" stop-opacity=".16"/>'
             f'<stop offset="1" stop-color="{TEAL}" stop-opacity="0"/></radialGradient>'
             f'<linearGradient id="wm" x1="0" x2="1"><stop offset="0" stop-color="{ACCENT}"/><stop offset="1" stop-color="{TEAL}"/></linearGradient>'
             f'<clipPath id="typeclip"><rect class="typed" x="640" y="70" width="330" height="30"/></clipPath></defs>')
    b.append(f'<rect x="1" y="1" width="{W-2}" height="{H-2}" rx="14" fill="url(#g1)"/>')
    b.append(f'<rect x="1" y="1" width="{W-2}" height="{H-2}" rx="14" fill="url(#g2)"/>')
    # wordmark
    b.append(f'<text x="56" y="112" class="word" fill="url(#wm)" style="fill:url(#wm)">Nova</text>')
    b.append(f'<text x="58" y="148" class="tag">A statically typed language for machine learning,</text>')
    b.append(f'<text x="58" y="174" class="tag">and NovaComp, the cloud IDE that runs it.</text>')
    # code card
    b.append(f'<rect x="620" y="40" width="524" height="164" rx="10" fill="{SURF}" stroke="{BORDER}"/>')
    for i, c in enumerate([PINK, AMBER, TEAL]):
        b.append(f'<circle cx="{640+i*16}" cy="58" r="4.5" fill="{c}" opacity=".8"/>')
    b.append(f'<text x="1124" y="62" class="tick mono" text-anchor="end">xor.nova</text>')
    code = [
        ("orbit novatorch.nn", None),
        ("ignite:", None),
        ("    Linear layer = Linear(2, 4)", None),
        ("    Tensor h = relu(layer.forward(X))", None),
        ("    backward(mse_loss(h @ W, Y))", None),
    ]
    kw = {"orbit": ACCENT, "ignite": ACCENT, "Linear": TEAL, "Tensor": TEAL}
    y = 92
    for line, _ in code:
        # simple highlight: first word coloured if keyword/type
        stripped = line.lstrip()
        indent = len(line) - len(stripped)
        first = stripped.split(" ")[0].rstrip(":")
        rest = stripped[len(first):]
        col = kw.get(first, TEXT)
        clip = ' clip-path="url(#typeclip)"' if y == 92 else ""
        b.append(f'<text x="640" y="{y}" class="mono" font-size="14"{clip} xml:space="preserve">'
                 f'<tspan fill="{MUTED}">{" " * indent}</tspan><tspan fill="{col}">{esc(first)}</tspan>'
                 f'<tspan fill="{TEXT}">{esc(rest)}</tspan></text>')
        y += 24
    b.append(f'<rect class="caret" x="{640 + len(code[-1][0]) * 8.45 + 4:.0f}" y="{y-38}" width="8" height="16" fill="{ACCENT}" opacity=".8"/>')
    # pipeline
    py = 290
    x0, x1 = 70, W - 70
    n = len(stages)
    xs = [x0 + i * (x1 - x0) / (n - 1) for i in range(n)]
    path = f"M {x0} {py} L {x1} {py}"
    b.append(f'<path d="{path}" stroke="{BORDER}" stroke-width="2" fill="none"/>')
    b.append(f'<path class="glow" d="{path}" stroke="url(#wm)" stroke-width="2" fill="none"/>')
    for i, (x, s) in enumerate(zip(xs, stages)):
        col = ACCENT if i < 5 else TEAL
        b.append(f'<circle cx="{x:.0f}" cy="{py}" r="9" fill="{BG}" stroke="{col}" stroke-width="2"/>')
        b.append(f'<circle cx="{x:.0f}" cy="{py}" r="3.5" fill="{col}"/>')
        b.append(f'<text x="{x:.0f}" y="{py+34}" class="stage" text-anchor="middle">{esc(s)}</text>')
    b.append(f'<text x="{x0}" y="{py-26}" class="tick">the compiler, end to end</text>')
    for d in (0, 1.4, 2.8):
        b.append(f'<circle class="pulse" r="5" fill="{TEAL}" style="offset-path: path(\'{path}\'); animation-delay: {d}s"/>')
    write("hero.svg", card(W, H, "\n".join(b), css))


# ── 2. CPU benchmarks: grouped horizontal bars on a log scale ────────────────────────────────
def cpu_bench():
    data = [  # docs/BENCHMARKS.md, best of 7, Intel Core Ultra 7 155H
        ("calls", "naive fib(30), 2.7M calls", [1004, 134, 56, 17]),
        ("int_loop", "3M-iteration integer loop", [134, 486, 54, 17]),
        ("vector_sum", "fill and sum a 1M vector", [93, 192, 64, 17]),
    ]
    langs = [("Nova", ACCENT), ("CPython 3.13", AMBER), ("Node 22", TEAL), ("C -O2", SKY)]
    W, H = 1000, 500
    L, R, T = 210, 70, 108
    bar, gap, ggap = 18, 5, 26
    lo, hi = 10, 2000
    sx = lambda v: L + (math.log10(v) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)) * (W - L - R)
    css = """
  .bar { transform-box: fill-box; transform-origin: left center; animation: grow 1.1s cubic-bezier(.2,.8,.2,1) both; }
  @keyframes grow { from { transform: scaleX(0); } to { transform: scaleX(1); } }
  .fade { animation: fade .6s ease-out both; }
  @keyframes fade { from { opacity: 0; } to { opacity: 1; } }
"""
    b = [f'<text x="32" y="44" class="title">CPU: Nova\'s interpreter next to CPython, Node and C</text>',
         f'<text x="32" y="68" class="sub">Wall-clock ms, best of 7, including process startup (log scale, shorter is faster). Intel Core Ultra 7 155H.</text>']
    # legend
    lx = 32
    for name, col in langs:
        b.append(f'<rect x="{lx}" y="82" width="12" height="12" rx="3" fill="{col}"/>'
                 f'<text x="{lx+18}" y="93" class="tick" style="fill:{TEXT}">{name}</text>')
        lx += 30 + len(name) * 7.4
    group_h = 4 * bar + 3 * gap
    plot_bottom = T + 3 * group_h + 2 * ggap
    for v in (10, 30, 100, 300, 1000):
        x = sx(v)
        b.append(f'<line x1="{x:.1f}" y1="{T-8}" x2="{x:.1f}" y2="{plot_bottom+6}" class="grid"/>'
                 f'<text x="{x:.1f}" y="{plot_bottom+24}" class="tick" text-anchor="middle">{v:,} ms</text>')
    delay = 0
    for gi, (name, desc, vals) in enumerate(data):
        gy = T + gi * (group_h + ggap)
        b.append(f'<text x="{L-16}" y="{gy+group_h/2-2}" class="lbl mono" text-anchor="end" font-weight="600">{name}</text>'
                 f'<text x="{L-16}" y="{gy+group_h/2+15}" class="tick" text-anchor="end">{esc(desc)}</text>')
        for li, ((lname, col), v) in enumerate(zip(langs, vals)):
            y = gy + li * (bar + gap)
            w = sx(v) - L
            b.append(f'<rect class="bar" style="animation-delay:{delay:.2f}s" x="{L}" y="{y}" width="{w:.1f}" height="{bar}" rx="4" fill="{col}"/>')
            label = f"{v:,} ms"
            if name == "int_loop" and lname == "Nova":
                label += "  ⚠ wrong answer (32-bit int overflow)"
            b.append(f'<text class="val fade" style="animation-delay:{delay+0.7:.2f}s" x="{L+w+8:.1f}" y="{y+bar-5}">{esc(label)}</text>')
            delay += 0.06
    b.append(f'<text x="32" y="{H-22}" class="sub">2–3.5× faster than CPython on loops and vectors; about 7.5× slower on function calls (per-call frame copies, see docs/BENCHMARKS.md).</text>')
    write("cpu-benchmarks.svg", card(W, H, "\n".join(b), css))


# ── 3. GPU: the matmul kernel ladder on 4096³ ────────────────────────────────────────────────
def gpu_ladder():
    steps = [  # compiler/kernels/README.md, A100, share of cuBLAS on 4096 x 4096 x 4096
        ("naive", "one thread per element", 13, MUTED),
        ("tiled", "32×32 shared-memory tiles", 27, SKY),
        ("blocked", "4×4 register blocking", 50, TEAL),
        ("pipelined", "cp.async double buffering", 82, ACCENT),
        ("Triton FP32", "autotuned, 4 stages", 95, PINK),
    ]
    W, H = 1000, 430
    L, B, T = 70, 330, 110
    cw = (W - L - 60) / len(steps)
    bw = cw * 0.5
    sy = lambda p: B - p / 100 * (B - T)
    css = """
  .col { transform-box: fill-box; transform-origin: center bottom; animation: rise 1s cubic-bezier(.2,.8,.2,1) both; }
  @keyframes rise { from { transform: scaleY(0); } to { transform: scaleY(1); } }
  .fade { animation: fade .5s ease-out both; }
  @keyframes fade { from { opacity: 0; } to { opacity: 1; } }
  .dash { stroke-dasharray: 6 5; }
"""
    b = [f'<text x="32" y="44" class="title">GPU: a 4096³ matmul, written four ways by hand, then in Triton</text>',
         f'<text x="32" y="68" class="sub">Share of cuBLAS\'s FP32 speed (19.0 TFLOP/s) on SOL\'s NVIDIA A100. Every kernel\'s result matches cuBLAS.</text>']
    for p in (0, 25, 50, 75, 100):
        y = sy(p)
        b.append(f'<line x1="{L}" y1="{y:.1f}" x2="{W-40}" y2="{y:.1f}" class="grid"/>'
                 f'<text x="{L-10}" y="{y+4:.1f}" class="tick" text-anchor="end">{p}%</text>')
    yc = sy(100)
    b.append(f'<line x1="{L}" y1="{yc:.1f}" x2="{W-40}" y2="{yc:.1f}" stroke="{AMBER}" class="dash"/>'
             f'<text x="{W-44}" y="{yc-8:.1f}" class="tick" text-anchor="end" style="fill:{AMBER}">cuBLAS</text>')
    pts = []
    for i, (name, desc, p, col) in enumerate(steps):
        cx = L + cw * i + cw / 2
        y = sy(p)
        d = i * 0.18
        b.append(f'<rect class="col" style="animation-delay:{d:.2f}s" x="{cx-bw/2:.1f}" y="{y:.1f}" width="{bw:.1f}" height="{B-y:.1f}" rx="6" fill="{col}" opacity=".92"/>')
        b.append(f'<text class="fade" style="animation-delay:{d+0.8:.2f}s" x="{cx:.1f}" y="{y-10:.1f}" text-anchor="middle" font-size="18" font-weight="700">{p}%</text>')
        b.append(f'<text x="{cx:.1f}" y="{B+24}" class="lbl" text-anchor="middle" font-weight="600">{name}</text>'
                 f'<text x="{cx:.1f}" y="{B+42}" class="tick" text-anchor="middle">{esc(desc)}</text>')
        pts.append((cx, y))
    write("gpu-matmul-ladder.svg", card(W, H, "\n".join(b), css))


# ── 4. GPU: pipelined CUDA vs Triton across the five cases ──────────────────────────────────
def gpu_cases():
    cases = [  # compiler/kernels/README.md, Triton table
        ("4096³", 82, 95, 84),
        ("32 × 512³, one A", 79, 99, 89),
        ("64 × 256³ batched", 77, 112, 98),
        ("1024³", 48, 82, 90),
        ("1000 × 999 × 1001", 43, 67, 100),
    ]
    series = [("pipelined CUDA (FP32)", ACCENT), ("Triton FP32", PINK), ("Triton TF32, tensor cores", TEAL)]
    W, H = 1000, 440
    L, B, T = 70, 330, 120
    maxp = 120
    gw = (W - L - 50) / len(cases)
    bw = gw * 0.22
    sy = lambda p: B - p / maxp * (B - T)
    css = """
  .col { transform-box: fill-box; transform-origin: center bottom; animation: rise .9s cubic-bezier(.2,.8,.2,1) both; }
  @keyframes rise { from { transform: scaleY(0); } to { transform: scaleY(1); } }
  .dash { stroke-dasharray: 6 5; }
"""
    b = [f'<text x="32" y="44" class="title">GPU: the same matmul across sizes, CUDA by hand against Triton</text>',
         f'<text x="32" y="68" class="sub">Share of cuBLAS on an A100 (FP32 against FP32, TF32 against TF32). Above 100% beats cuBLAS.</text>']
    lx = 32
    for name, col in series:
        b.append(f'<rect x="{lx}" y="84" width="12" height="12" rx="3" fill="{col}"/>'
                 f'<text x="{lx+18}" y="95" class="tick" style="fill:{TEXT}">{name}</text>')
        lx += 36 + len(name) * 7.0
    for p in (0, 40, 80, 120):
        y = sy(p)
        b.append(f'<line x1="{L}" y1="{y:.1f}" x2="{W-30}" y2="{y:.1f}" class="grid"/>'
                 f'<text x="{L-10}" y="{y+4:.1f}" class="tick" text-anchor="end">{p}%</text>')
    yc = sy(100)
    b.append(f'<line x1="{L}" y1="{yc:.1f}" x2="{W-30}" y2="{yc:.1f}" stroke="{AMBER}" class="dash"/>')
    b.append(f'<line x1="{lx}" y1="90" x2="{lx+22}" y2="90" stroke="{AMBER}" stroke-width="2" class="dash"/>'
             f'<text x="{lx+28}" y="95" class="tick" style="fill:{AMBER}">cuBLAS = 100%</text>')
    for gi, (name, *vals) in enumerate(cases):
        gx = L + gi * gw + gw / 2
        for si, ((sname, col), v) in enumerate(zip(series, vals)):
            x = gx + (si - 1) * (bw + 4) - bw / 2
            y = sy(v)
            d = gi * 0.12 + si * 0.05
            b.append(f'<rect class="col" style="animation-delay:{d:.2f}s" x="{x:.1f}" y="{y:.1f}" width="{bw:.1f}" height="{B-y:.1f}" rx="4" fill="{col}"/>')
            b.append(f'<text x="{x+bw/2:.1f}" y="{y-6:.1f}" class="tick" text-anchor="middle" style="fill:{TEXT}; paint-order: stroke; stroke: {BG}; stroke-width: 4px">{v}</text>')
        b.append(f'<text x="{gx:.1f}" y="{B+24}" class="lbl mono" text-anchor="middle">{esc(name)}</text>')
    b.append(f'<text x="32" y="{H-24}" class="sub">Triton TF32 reaches 111 TFLOP/s on 4096³. Its autotuner picks deeper pipelines and smaller tiles than the hand-written kernel.</text>')
    write("gpu-matmul-cases.svg", card(W, H, "\n".join(b), css))


hero()
cpu_bench()
gpu_ladder()
gpu_cases()
