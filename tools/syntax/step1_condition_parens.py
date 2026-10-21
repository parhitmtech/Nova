#!/usr/bin/env python3
"""Syntax migration, step 1: put if / elif / while conditions in parentheses.

    if x < 5 {            ->  if (x < 5) {
    elif a == b {         ->  elif (a == b) {
    while i < n {         ->  while (i < n) {
    } while i < n ;       ->  } while (i < n) ;

A condition that is already fully wrapped, like (x < 5), is left alone; one that only starts with a
parenthesis, like (a + b) > c, is wrapped. Comments and string literals are skipped. Works on .nova
files and on Nova code embedded in other files (JavaScript conditions are already parenthesized, so
they are left alone).

Usage: python tools/syntax/step1_condition_parens.py FILE...   (rewrites the files in place)
"""
import re
import sys

KEYWORD = re.compile(r"\b(if|elif|while)\b")


def code_mask(line, hash_comments):
    """For each character: True if it is code (not inside a string literal or a comment)."""
    mask = [True] * len(line)
    in_str = False
    i = 0
    while i < len(line):
        c = line[i]
        if in_str:
            mask[i] = False
            if c == "\\":
                if i + 1 < len(line):
                    mask[i + 1] = False
                i += 2
                continue
            if c == '"':
                in_str = False
        elif c == '"':
            in_str = True
            mask[i] = False
        elif line.startswith("//", i) or (hash_comments and c == "#"):
            for j in range(i, len(line)):
                mask[j] = False
            break
        i += 1
    return mask


def fully_wrapped(cond):
    """True if cond is '( ... )' with the first '(' closed by the last ')'."""
    if not (cond.startswith("(") and cond.endswith(")")):
        return False
    depth = 0
    for i, c in enumerate(cond):
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0 and i != len(cond) - 1:
                return False
    return depth == 0


def convert_line(line, hash_comments):
    mask = code_mask(line, hash_comments)
    out = []
    pos = 0
    for m in KEYWORD.finditer(line):
        if m.start() < pos or not mask[m.start()]:
            continue
        # a do-while tail: "} while cond ;" ends at ';', every other condition at the block's '{'
        do_while = m.group(1) == "while" and line[:m.start()].rstrip().endswith("}")
        end_char = ";" if do_while else "{"
        depth = 0
        end = -1
        for j in range(m.end(), len(line)):
            if not mask[j]:
                continue
            c = line[j]
            if c in "([":
                depth += 1
            elif c in ")]":
                depth -= 1
            elif c == end_char and depth == 0:
                end = j
                break
        if end < 0:
            continue
        cond = line[m.end():end].strip()
        if not cond or fully_wrapped(cond):
            continue
        out.append(line[pos:m.end()])
        out.append(" (" + cond + ") ")
        pos = end
    out.append(line[pos:])
    return "".join(out)


def main(paths):
    for path in paths:
        with open(path, encoding="utf-8", newline="") as f:
            text = f.read()
        hash_comments = path.endswith(".nova")
        lines = text.split("\n")
        new_lines = [convert_line(l, hash_comments) for l in lines]
        changed = sum(1 for a, b in zip(lines, new_lines) if a != b)
        if changed:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write("\n".join(new_lines))
        print(f"{path}: {changed} line(s) changed")


if __name__ == "__main__":
    main(sys.argv[1:])
