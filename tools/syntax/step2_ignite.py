#!/usr/bin/env python3
"""Syntax migration, step 2: the entry point 'main() { ... }' becomes 'ignite { ... }'.

    main() {              ->  ignite {
    main()                ->  ignite
    {                         {

'def main()' (an ordinary function, still accepted as an entry point) and C-style 'int main()' /
'void main()' are left alone. Works on .nova files and on Nova code embedded in other files.

Usage: python tools/syntax/step2_ignite.py FILE...   (rewrites the files in place)
"""
import re
import sys

# 'main()' not preceded by a word character, '.', or the words def / int / void, followed by '{'
ENTRY = re.compile(r"(?<![\w.])(?<!def )(?<!int )(?<!void )main\s*\(\s*\)(\s*)\{")


def main(paths):
    for path in paths:
        with open(path, encoding="utf-8", newline="") as f:
            text = f.read()
        new, count = ENTRY.subn(lambda m: "ignite" + m.group(1) + "{", text)
        if count:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(new)
        print(f"{path}: {count} entry point(s) changed")


if __name__ == "__main__":
    main(sys.argv[1:])
