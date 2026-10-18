#!/usr/bin/env python3
"""Syntax migration, step 3: braces and semicolons become ':' and indentation.

    def f(int x) -> int {         def f(int x) -> int:
        if (x > 0) { return 1 ; }      if (x > 0): return 1
        return 0 ;                ->     return 0
    }
    ignite {                      ignite:
        print(f(2)) ; // two          print(f(2))  # two
    }

- A block whose body is one statement on the same line stays a one-liner: 'if (x): y = 1'.
- An empty block becomes 'pass'. '} else {' splits into two lines; 'do { } while (c) ;' becomes
  'do:' + body, then 'while (c)'. Semicolons inside a for header are kept.
- '//' comments become '#'. Blank lines, trailing comments and the spacing inside each line are kept.
- import lines lose their ';'.

Usage:
    python tools/syntax/step3_indentation.py FILE.nova ...        (rewrites the files in place)
    python tools/syntax/step3_indentation.py --stdin < in > out   (one program, for embedded code)
"""
import re
import sys

OPERATORS = ["->", "==", "!=", "<=", ">=", "+=", "-=", "*=", "/=", "<>"]


class Tok:
    def __init__(self, kind, text, line, start, end):
        self.kind, self.text, self.line, self.start, self.end = kind, text, line, start, end

    def __repr__(self):
        return f"{self.kind}:{self.text!r}@{self.line}"


def tokenize(src):
    toks = []
    i, line, n = 0, 1, len(src)
    line_start = True
    while i < n:
        c = src[i]
        if c == "\n":
            line += 1
            i += 1
            line_start = True
            continue
        if c in " \t\r":
            i += 1
            continue
        if line_start and src.startswith("import ", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            toks.append(Tok("import", src[i:j], line, i, j))
            i = j
            continue
        line_start = False
        if src.startswith("//", i) or c == "#":
            j = src.find("\n", i)
            j = n if j < 0 else j
            body = src[i + 2:j] if c == "/" else src[i + 1:j]
            toks.append(Tok("comment", "#" + body.rstrip(), line, i, j))
            i = j
            continue
        if c == '"':
            j = i + 1
            while j < n and src[j] != '"':
                j += 2 if src[j] == "\\" else 1
            j += 1
            toks.append(Tok("str", src[i:j], line, i, j))
            i = j
            continue
        m = re.match(r"[A-Za-z_]\w*|\d[\d_]*(\.\d+)?([eE][+-]?\d+)?", src[i:])
        if m:
            toks.append(Tok("word", m.group(0), line, i, i + len(m.group(0))))
            i += len(m.group(0))
            continue
        op = next((o for o in OPERATORS if src.startswith(o, i)), c)
        toks.append(Tok("op", op, line, i, i + len(op)))
        i += len(op)
    return toks


def matching_brace(toks, i):
    depth = 0
    for j in range(i, len(toks)):
        if toks[j].text == "{" and toks[j].kind == "op":
            depth += 1
        elif toks[j].text == "}" and toks[j].kind == "op":
            depth -= 1
            if depth == 0:
                return j
    return len(toks) - 1


def convert(src):
    toks = tokenize(src)
    src_lines = src.split("\n")
    out = []
    buf = ""            # the output line being built
    indent = 0
    paren = 0
    pending = False     # buf holds a finished statement (flush unless a same-line comment follows)
    last = None         # last token written to buf (for spacing)
    last_line = 0       # source line of the last token written anywhere
    block_starts = []   # len(out) when each open block started (to detect empty blocks)

    def blank_lines_before(line):
        if not last_line:
            return 0
        return sum(1 for k in range(last_line, line - 1) if not src_lines[k].strip())

    def flush():
        nonlocal buf, pending, last
        if buf.strip():
            out.append("    " * indent + buf.rstrip())
        buf, pending, last = "", False, None

    def write(t, gap=None):
        nonlocal buf, last, last_line
        if not buf:
            for _ in range(min(blank_lines_before(t.line), 2)):
                out.append("")
            buf = t.text
        else:
            if gap is None:
                between = src[last.end:t.start] if last is not None else " "
                gap = between if last is not None and last.line == t.line and not between.strip() else " "
            buf += gap + t.text
        last, last_line = t, t.line

    i = 0
    while i < len(toks):
        t = toks[i]
        if pending and not (t.kind == "comment" and last_line == t.line):
            flush()
        if t.kind == "import":
            flush()
            for _ in range(min(blank_lines_before(t.line), 2)):
                out.append("")
            out.append("    " * indent + t.text.rstrip().rstrip(";").rstrip())
            last_line = t.line
        elif t.kind == "comment":
            if buf and last_line == t.line:
                write(t, gap="  ")
                flush()
            else:
                flush()
                write(t)
                flush()
        elif t.kind == "op" and t.text == ";" and paren == 0:
            pending = True
            last_line = t.line
        elif t.kind == "op" and t.text == "{" and paren == 0:
            j = matching_brace(toks, i)
            inner = toks[i + 1:j]
            semis = [k for k, x in enumerate(inner) if x.kind == "op" and x.text == ";"]
            one_line = (toks[j].line == t.line and all(x.line == t.line for x in inner)
                        and not any(x.kind in ("comment", "import") or x.text in "{}" for x in inner)
                        and len(semis) <= 1 and (not inner or semis == [len(inner) - 1]))
            if not buf.rstrip().endswith(":"):
                buf = buf.rstrip() + ":"
            if one_line:
                body = inner[:-1]
                if not body:
                    buf += " pass"
                else:
                    write(body[0], gap=" ")
                    for x in body[1:]:
                        write(x)
                pending = True
                last_line = t.line
                i = j
            else:
                pending = True
                last_line = t.line
                flush()
                indent += 1
                block_starts.append(len(out))
        elif t.kind == "op" and t.text == "}" and paren == 0:
            flush()
            if block_starts and block_starts.pop() == len(out):
                out.append("    " * indent + "pass")
            indent -= 1
            last_line = t.line
        else:
            if t.kind == "op" and t.text in "([":
                paren += 1
            elif t.kind == "op" and t.text in ")]":
                paren -= 1
            write(t)
        i += 1
    flush()
    while out and not out[-1].strip():
        out.pop()
    return "\n".join(out) + "\n"


def main(args):
    if args == ["--stdin"]:
        sys.stdout.write(convert(sys.stdin.read()))
        return
    for path in args:
        with open(path, encoding="utf-8", newline="") as f:
            src = f.read()
        new = convert(src)
        if new != src:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(new)
        print(f"{path}: converted")


if __name__ == "__main__":
    main(sys.argv[1:])
