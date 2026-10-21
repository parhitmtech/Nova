#!/usr/bin/env python3
"""Syntax migration, step 7: 'orbit' modules and constructors.

    import <novatorch.nn>        ->  orbit novatorch.nn
    import "model.nova"          ->  orbit model
    import hf                    ->  orbit hf
    Linear layer                 ->  Linear layer = Linear(2, 4)
    layer.init(2, 4)                 (the init call merges into the declaration)

A declaration 'Cls v' merges with an 'v.init(args)' on the next code line (blank and comment lines in
between are kept). 'Cls v' with no init call stays: a no-argument init still runs automatically.

Usage: python tools/syntax/step7_orbit_constructors.py FILE...   (rewrites the files in place)
"""
import re
import sys

IMPORT = re.compile(r'^(\s*)import\s+(?:<([\w.]+)>|"([\w./]+?)(?:\.nova)?"|(hf|sol)\b)\s*;?\s*(#.*)?$')
DECL = re.compile(r"^(\s*)([A-Z]\w*)\s+(\w+)\s*(#.*)?$")


def convert(text):
    lines = text.split("\n")
    out = []
    i = 0
    changes = 0
    while i < len(lines):
        line = lines[i]
        m = IMPORT.match(line)
        if m:
            name = m.group(2) or (m.group(3) or "").replace("/", ".") or m.group(4)
            comment = "  " + m.group(5) if m.group(5) else ""
            out.append(f"{m.group(1)}orbit {name}{comment}")
            changes += 1
            i += 1
            continue
        m = DECL.match(line)
        if m and m.group(2) != "Tensor":
            indent, cls, var = m.group(1), m.group(2), m.group(3)
            j = i + 1
            while j < len(lines) and (not lines[j].strip() or lines[j].strip().startswith("#")):
                j += 1
            init = re.match(rf"^{re.escape(indent)}{var}\.init\((.*)\)\s*(#.*)?$", lines[j]) if j < len(lines) else None
            if init:
                comment = "  " + init.group(2) if init.group(2) else ""
                out.append(f"{indent}{cls} {var} = {cls}({init.group(1)}){comment}")
                out.extend(lines[i + 1:j])  # blank / comment lines between them
                changes += 1
                i = j + 1
                continue
        out.append(line)
        i += 1
    return "\n".join(out), changes


def main(paths):
    for path in paths:
        with open(path, encoding="utf-8", newline="") as f:
            src = f.read()
        new, changes = convert(src)
        if changes:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(new)
        print(f"{path}: {changes} change(s)")


if __name__ == "__main__":
    main(sys.argv[1:])
