#!/usr/bin/env python3
"""make_bench.py - build the Athena Aura Bench page from reference/aurora.js.

The bench is the live in-browser simulator of the panel (state buttons, the
session script, the tween and fade sliders, the live parameter row). It is
bench.template.html with reference/aurora.js inlined at the @@AURORA_JS@@
marker, so it always runs the same numbers as aura.c mirrors.

    python3 host/bench/make_bench.py --out /path/athena-aura-bench.html
    python3 host/bench/make_bench.py --check /path/athena-aura-bench.html

--out writes the page; publish it with the Artifact tool to the existing bench
URL (see the aura-bench memory note or CLAUDE.md), never as a new artifact.
--check exits 0 when the given page carries the current aurora.js verbatim,
1 when it is stale. Keep the state descriptions in the template's LOOK table
by hand when a state's character changes; they are prose, not derived.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TEMPLATE = os.path.join(HERE, 'bench.template.html')
AURORA = os.path.normpath(os.path.join(HERE, '..', '..', 'reference', 'aurora.js'))
MARKER = '@@AURORA_JS@@'


def read(p):
    with open(p, encoding='utf-8') as fh:
        return fh.read()


def build():
    tpl, js = read(TEMPLATE), read(AURORA)
    lines = [l for l in tpl.split('\n') if MARKER in l]
    if len(lines) != 1:
        sys.exit(f'make_bench: expected one {MARKER} line in the template, found {len(lines)}')
    if 'root.Aurora = api' not in js:
        sys.exit('make_bench: reference/aurora.js no longer exports window.Aurora; the bench needs it')
    if '</script>' in js:
        sys.exit('make_bench: aurora.js contains "</script>", which would end the inline block early')
    return tpl.replace(lines[0], js.rstrip('\n')), js


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--out', help='write the built page here')
    g.add_argument('--check', help='is this built page carrying the current aurora.js?')
    a = ap.parse_args()
    page, js = build()
    if a.out:
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        with open(a.out, 'w', encoding='utf-8') as fh:
            fh.write(page)
        print(f'make_bench: wrote {a.out} ({len(page.encode()) // 1024} KB, aurora.js {len(js.encode()) // 1024} KB inlined)')
    else:
        got = read(a.check)
        if js.rstrip('\n') in got:
            print(f'make_bench: {a.check} is current')
        else:
            print(f'make_bench: {a.check} is STALE, rebuild with --out and republish')
            sys.exit(1)


if __name__ == '__main__':
    main()
