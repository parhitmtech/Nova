#!/usr/bin/env python3
"""Syntax migration, step 5: tensors get the Tensor type instead of int.

The compiler does the type inference: this script runs 'nova --check' on a file, reads the type
errors, rewrites the declaration each one points at, and repeats until the file checks cleanly.

    cannot assign Tensor to int variable 'x'            int x = ...      ->  Tensor x = ...
    argument N of 'op' must be a Tensor, but 'x' is int  (x's declaration, a local or a parameter)
    'f' returns Tensor but is declared '-> int'          def f(...) -> int:  ->  -> Tensor
    'Cls.m' returns Tensor ...                           (the method m of class Cls)
    cannot assign Tensor to int field 'W' of class 'C'   int W (in class C)  ->  Tensor W
    argument N of 'f' expects int but got Tensor         parameter N of f (or Cls.m)  ->  Tensor
    cannot assign int to Tensor variable 'h'             Tensor h = 0     ->  Tensor h

Convert the stdlib first, so the errors left in a program are its own.

Usage: NOVA_HOME=compiler python tools/syntax/step5_tensor_types.py --nova compiler/build/nova FILE...
"""
import re
import subprocess
import sys

ERR = re.compile(r"^Error at line (\d+): (.*)$")


def check(nova, path):
    r = subprocess.run([nova, "--check", path], capture_output=True, text=True)
    errors = []
    for line in r.stderr.splitlines():
        m = ERR.match(line.strip())
        if m:
            errors.append((int(m.group(1)), m.group(2)))
    return errors


def find_back(lines, line_no, pattern):
    """Index of the nearest line at or above line_no (1-based) matching pattern, or -1."""
    for i in range(min(line_no, len(lines)) - 1, -1, -1):
        if pattern.search(lines[i]):
            return i
    return -1


def def_line(lines, label):
    """Index of the 'def' line of a function 'f' or a method 'Cls.m', or -1."""
    if "." in label:
        cls, meth = label.split(".", 1)
        c = next((i for i, l in enumerate(lines) if re.match(rf"\s*class\s+{cls}\b", l)), -1)
        if c < 0:
            return -1
        for i in range(c + 1, len(lines)):
            if re.match(r"\S", lines[i]) and not lines[i].startswith("#"):
                break  # left the class body
            if re.match(rf"\s*def\s+{meth}\s*\(", lines[i]):
                return i
        return -1
    return next((i for i, l in enumerate(lines) if re.match(rf"\s*def\s+{label}\s*\(", l)), -1)


def retype_name(lines, line_no, name):
    """Change the 'int' declaring name (a local or a parameter) at or above line_no to Tensor."""
    pat = re.compile(rf"\bint(\s+{re.escape(name)}\b)")
    i = find_back(lines, line_no, pat)
    if i < 0:
        return False
    lines[i] = pat.sub(r"Tensor\1", lines[i], count=1)
    return True


def apply(lines, line_no, msg):
    m = re.match(r"cannot assign Tensor to int variable '(\w+)'", msg)
    if m:
        return retype_name(lines, line_no, m.group(1))
    m = re.match(r"argument \d+ of '\w+' must be a Tensor, but '(\w+)' is int", msg)
    if m:
        return retype_name(lines, line_no, m.group(1))
    m = re.match(r"'([\w.]+)' returns Tensor but is declared '-> int'", msg)
    if m:
        i = def_line(lines, m.group(1))
        if i >= 0 and "-> int" in lines[i]:
            lines[i] = lines[i].replace("-> int", "-> Tensor", 1)
            return True
        return False
    m = re.match(r"cannot assign Tensor to int field '(\w+)' of class '(\w+)'", msg)
    if m:
        field, cls = m.groups()
        c = next((i for i, l in enumerate(lines) if re.match(rf"\s*class\s+{cls}\b", l)), -1)
        for i in range(c + 1, len(lines)) if c >= 0 else []:
            if re.match(rf"\s*int\s+{field}\s*(#.*)?$", lines[i]):
                lines[i] = re.sub(r"\bint\b", "Tensor", lines[i], count=1)
                return True
        return False
    m = re.match(r"argument (\d+) of '([\w.]+)' expects int but got Tensor", msg)
    if m:
        n, label = int(m.group(1)), m.group(2)
        i = def_line(lines, label)
        if i < 0:
            return False
        params = re.search(r"\((.*)\)", lines[i])
        parts = params.group(1).split(",") if params else []
        if n - 1 < len(parts) and re.match(r"\s*int\s", parts[n - 1]):
            parts[n - 1] = re.sub(r"\bint\b", "Tensor", parts[n - 1], count=1)
            lines[i] = lines[i][:params.start(1)] + ",".join(parts) + lines[i][params.end(1):]
            return True
        return False
    m = re.match(r"cannot assign int to Tensor variable '(\w+)'", msg)
    if m:
        pat = re.compile(rf"(\bTensor\s+{re.escape(m.group(1))})\s*=\s*0\s*(#.*)?$")
        i = find_back(lines, line_no, pat)
        if i >= 0:
            lines[i] = pat.sub(lambda k: k.group(1) + ("  " + k.group(2) if k.group(2) else ""), lines[i])
            return True
    return False


def convert(nova, path):
    with open(path, encoding="utf-8", newline="") as f:
        lines = f.read().split("\n")
    changes = 0
    for _ in range(30):
        errors = check(nova, path)
        changed = False
        for line_no, msg in errors:
            if apply(lines, line_no, msg):
                changed = True
                changes += 1
        if not changed:
            break
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write("\n".join(lines))
    left = [e for e in check(nova, path) if "no entry point" not in e[1]]
    return changes, left


def main(args):
    nova = args[args.index("--nova") + 1]
    files = [a for a in args if a not in ("--nova", nova)]
    for path in files:
        changes, left = convert(nova, path)
        status = "ok" if not left else f"{len(left)} error(s) left, first: line {left[0][0]}: {left[0][1]}"
        print(f"{path}: {changes} change(s), {status}")


if __name__ == "__main__":
    main(sys.argv[1:])
