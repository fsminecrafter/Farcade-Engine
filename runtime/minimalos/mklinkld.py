#!/usr/bin/env python3
"""Writes the SDK's link.ld with .ctors added to the .init_array output section.

x86_64-elf-g++ (a bare-metal ELF target) puts the constructors of C++ globals
in .ctors, not .init_array. The SDK's link.ld only collects .init_array (which
crt0.c walks before main), so without this the constructors never run.

    mklinkld.py <sdk link.ld> <output>
"""
import sys

src, dst = sys.argv[1], sys.argv[2]
text = open(src).read()
old_prio = "KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))"
old_plain = "KEEP(*(.init_array))"
if old_prio not in text or old_plain not in text:
    sys.exit(f"{src}: .init_array rules not found; update runtime/minimalos/mklinkld.py")
text = text.replace(old_prio, old_prio + "\n        KEEP(*(SORT_BY_INIT_PRIORITY(.ctors.*)))")
text = text.replace(old_plain, old_plain + "\n        KEEP(*(.ctors))")
open(dst, "w").write(text)
