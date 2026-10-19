#!/usr/bin/env python3
"""Cleanup after syntax step 6: rewrite tensor built-in calls as operators and short names.

    tensor_add(a, b)      ->  a + b          tensor_get(t, i, j)      ->  t[i, j]
    tensor_sub(a, b)      ->  a - b          tensor_set(t, i, j, v)   ->  t[i, j] = v   (a statement)
    tensor_mul(a, b)      ->  a * b          tensor_rows(t)           ->  t.rows   (also cols)
    matmul(a, b)          ->  a @ b          tensor_T(x)              ->  x.T
    tensor_scale(t, k)    ->  k * t          tensor_zeros(...)        ->  zeros(...)  (ones, randn, xavier)

Nested calls are rewritten inside out and parenthesized by precedence, so
tensor_sub(W, tensor_scale(dW, lr)) becomes W - lr * dW and tensor_T(matmul(a, b)) becomes (a @ b).T.
Postfixes (.T, [i, j], .rows) are not applied to a field like self.W (not supported yet): those calls
are kept. Comments and the text outside rewritten calls are left as they are.

Usage: python tools/syntax/cleanup_operators.py FILE...   (rewrites the files in place)
"""
import re
import sys

ATOM = 100  # precedence of a name, literal, call or postfix expression
BINOPS = {"or": 1, "and": 2, "==": 4, "!=": 4, "<": 4, ">": 4, "<=": 4, ">=": 4,
          "+": 5, "-": 5, "*": 6, "/": 6, "%": 6, "@": 6}
RENAME = {"tensor_zeros": "zeros", "tensor_ones": "ones", "tensor_randn": "randn", "tensor_xavier": "xavier"}
TARGETS = {"tensor_add", "tensor_sub", "tensor_mul", "matmul", "tensor_scale", "tensor_T", "tensor_get",
           "tensor_rows", "tensor_cols", *RENAME}
TOKEN = re.compile(r'"(?:\\.|[^"\\])*"|\d+(?:\.\d+)?(?:[eE][+-]?\d+)?|[A-Za-z_]\w*|==|!=|<=|>=|->|\S')


def split_comment(line):
    in_str = False
    for i, c in enumerate(line):
        if c == '"' and (i == 0 or line[i - 1] != "\\"):
            in_str = not in_str
        elif c == "#" and not in_str:
            return line[:i], line[i:]
    return line, ""


def closing(toks, i):
    """Index of the bracket that closes toks[i]."""
    depth = 0
    for j in range(i, len(toks)):
        if toks[j] in "([":
            depth += 1
        elif toks[j] in ")]":
            depth -= 1
            if depth == 0:
                return j
    return len(toks) - 1


def split_args(toks):
    args, depth, cur = [], 0, []
    for t in toks:
        if t in "([":
            depth += 1
        elif t in ")]":
            depth -= 1
        if t == "," and depth == 0:
            args.append(cur)
            cur = []
        else:
            cur.append(t)
    if cur:
        args.append(cur)
    return args


KEYWORDS = {"if", "elif", "while", "switch", "return", "and", "or", "not", "case", "for", "print"}


def join(parts):
    """Join (text) pieces with Nova's usual spacing: f(x), t[i], a.b, 'if (', 'x:'."""
    out = ""
    for p in parts:
        last_word = re.search(r"(\w+)$", out)
        keyword_before = last_word is not None and last_word.group(1) in KEYWORDS - {"print"}
        if not out:
            out = p
        elif p in (")", "]", ",", ".", ":") or out.endswith(("(", "[", ".")):
            out += p
        elif p in ("(", "[") and re.search(r"[\w)\]]$", out) and not keyword_before:
            out += p
        else:
            out += " " + p
    return re.sub(r",(?=\S)", ", ", out)


def prec_of(units):
    """Lowest top-level operator precedence in a list of (text, prec) units."""
    p = ATOM
    for k, (text, up) in enumerate(units):
        if up is None and text in BINOPS:
            if text == "-" and (k == 0 or (units[k - 1][1] is None and units[k - 1][0] in BINOPS)):
                p = min(p, 7)  # unary minus
            else:
                p = min(p, BINOPS[text])
        elif up is None and text == "not":
            p = min(p, 3)
    return p


def wrap(text, p, need):
    return f"({text})" if p < need else text


def postfixable(text, p):
    # a dotted name such as self.W cannot take .T / [i, j] / .rows yet
    return not (p == ATOM and re.fullmatch(r"[\w.]+", text) and "." in text)


def rewrite(name, args):
    """args: list of (text, prec). Returns (text, prec) for the rewritten call."""
    def call():
        return f"{RENAME.get(name, name)}({', '.join(a for a, _ in args)})", ATOM
    if name in RENAME:
        return call()
    if name in ("tensor_add", "tensor_sub", "tensor_mul", "matmul") and len(args) == 2:
        op, p = {"tensor_add": ("+", 5), "tensor_sub": ("-", 5), "tensor_mul": ("*", 6), "matmul": ("@", 6)}[name]
        (a, pa), (b, pb) = args
        return f"{wrap(a, pa, p)} {op} {wrap(b, pb, p + 1)}", p
    if name == "tensor_scale" and len(args) == 2:
        (t, pt), (k, pk) = args
        return f"{wrap(k, pk, 7)} * {wrap(t, pt, 7)}", 6
    if name in ("tensor_T", "tensor_rows", "tensor_cols") and len(args) == 1:
        (x, px), = args
        if not postfixable(x, px):
            return call()
        member = {"tensor_T": "T", "tensor_rows": "rows", "tensor_cols": "cols"}[name]
        return f"{wrap(x, px, ATOM)}.{member}", ATOM
    if name == "tensor_get" and len(args) >= 2:
        t, pt = args[0]
        if not postfixable(t, pt):
            return call()
        return f"{wrap(t, pt, ATOM)}[{', '.join(a for a, _ in args[1:])}]", ATOM
    return call()


def process(toks):
    """Rewrite a token list (one expression or statement). Returns (text, prec)."""
    units = []  # (text, prec or None for a raw operator/punctuation token)
    i = 0
    while i < len(toks):
        t = toks[i]
        if (re.fullmatch(r"[A-Za-z_]\w*", t) and t not in KEYWORDS - {"print"}
                and i + 1 < len(toks) and toks[i + 1] == "("):
            j = closing(toks, i + 1)
            args = [process(a) for a in split_args(toks[i + 2:j])]
            if t in TARGETS:
                units.append(rewrite(t, args))
            else:
                units.append((f"{t}({', '.join(a for a, _ in args)})", ATOM))
            i = j + 1
            continue
        if t in ("(", "["):
            j = closing(toks, i)
            inner = [process(a) for a in split_args(toks[i + 1:j])]
            text = ", ".join(a for a, _ in inner)
            # a parenthesized single expression keeps its parentheses; it is an atom from outside
            units.append((f"{t}{text}{')' if t == '(' else ']'}", ATOM if t == "(" else None))
            i = j + 1
            continue
        units.append((t, ATOM if re.fullmatch(r'[\w.]+|"(?:\\.|[^"\\])*"', t) else None))
        i += 1
    # merge postfix pieces (name followed by [..] or .x) into atoms
    merged = []
    for text, p in units:
        if merged and (text.startswith("[") or text == ".") and merged[-1][1] == ATOM:
            merged[-1] = (merged[-1][0] + text, ATOM)
        elif merged and merged[-1][0].endswith(".") and merged[-1][1] == ATOM:
            merged[-1] = (merged[-1][0] + text, ATOM)
        else:
            merged.append((text, p if p is not None else None))
    text = join([u for u, _ in merged])
    return text, prec_of(merged)


def convert_line(line):
    code, comment = split_comment(line)
    if not any(re.search(rf"\b{t}\s*\(", code) for t in TARGETS | {"tensor_set"}):
        return line
    indent = code[:len(code) - len(code.lstrip())]
    body = code.strip()
    toks = TOKEN.findall(body)
    # tensor_set(t, i, j, v) as a statement -> t[i, j] = v
    if toks[:2] == ["tensor_set", "("] and closing(toks, 1) == len(toks) - 1:
        args = [process(a) for a in split_args(toks[2:-1])]
        if len(args) >= 3 and re.fullmatch(r"[A-Za-z_]\w*", args[0][0]):
            new = f"{args[0][0]}[{', '.join(a for a, _ in args[1:-1])}] = {args[-1][0]}"
            return indent + new + (("  " + comment) if comment else "")
    # keep a block header's 'keyword (' and trailing ':' intact
    text, _ = process(toks)
    text = re.sub(r"\s+:$", ":", text)
    gap = re.search(r"\s*$", code).group(0) if comment else ""
    return indent + text + (gap if gap else ("  " if comment else "")) + comment


def main(paths):
    for path in paths:
        with open(path, encoding="utf-8", newline="") as f:
            lines = f.read().split("\n")
        new = [convert_line(l) for l in lines]
        changed = sum(a != b for a, b in zip(lines, new))
        if changed:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write("\n".join(new))
        print(f"{path}: {changed} line(s) changed")


if __name__ == "__main__":
    main(sys.argv[1:])
