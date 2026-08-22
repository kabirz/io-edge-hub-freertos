#!/usr/bin/env python3
"""Print memory region usage from ELF file.
Usage: python tools/print_mem.py <elf> [flash_size] [ram_size] [ccmram_size]
If sizes not provided, reads from linked .ld file or ELF program headers.
"""
import subprocess
import sys
import re
import os

def parse_size(s):
    s = s.strip()
    if s.upper().startswith('0X'):
        return int(s, 16)
    if s.upper().endswith('K'):
        return int(s[:-1]) * 1024
    return int(s)

def fmt_size(n):
    if n >= 1024 * 1024:
        return '%.1f MiB' % (n / 1024.0 / 1024.0)
    if n >= 1024:
        return '%.1f KiB' % (n / 1024.0)
    return '%d B' % n

def read_memory_from_ld(ld_path):
    """Parse MEMORY{} block from linker script."""
    if not os.path.exists(ld_path):
        return {}
    with open(ld_path) as f:
        content = f.read()
    m = re.search(r'MEMORY\s*\{(.*?)\}', content, re.S)
    if not m:
        return {}
    regions = {}
    for line in m.group(1).splitlines():
        # FLASH (rx) : ORIGIN = 0x8010200, LENGTH = 0x6FE00
        km = re.match(r'\s*(\S+)\s*\(.*?\)\s*:\s*ORIGIN\s*=\s*(0x[0-9a-fA-F]+)\s*,\s*LENGTH\s*=\s*([0-9a-fA-FxKk]+)', line)
        if km:
            name = km.group(1)
            length = parse_size(km.group(3))
            regions[name] = length
    return regions

def read_sections(elf):
    r = subprocess.run(['arm-none-eabi-objdump', '-h', elf],
                       capture_output=True, text=True)
    sections = {}
    for line in r.stdout.splitlines():
        m = re.match(r'^\s*\d+\s+(\S+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)', line)
        if m:
            sections[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))
    return sections

def main():
    elf = sys.argv[1]
    sections = read_sections(elf)

    # 从 ELF 旁边的 .ld 文件读取 MEMORY 区域
    elf_dir = os.path.dirname(os.path.abspath(elf))
    elf_name = os.path.basename(elf).replace('.elf', '.ld')
    ld_path = os.path.join(elf_dir, elf_name)
    mem = read_memory_from_ld(ld_path)

    flash_used = sum(sections.get(s, (0, 0))[0] for s in
        ('.isr_vector', '.text', '.rodata', '.ARM.extab', '.ARM',
         '.preinit_array', '.init_array', '.fini_array'))
    flash_used += sections.get('.data', (0, 0))[0]

    ram_used = (sections.get('.data', (0, 0))[0] + sections.get('.bss', (0, 0))[0]
                + 0x200 + 0x400)
    ccm_used = sections.get('.ccmram', (0, 0))[0]

    flash_total = mem.get('FLASH', 0)
    ram_total = mem.get('RAM', 0)
    ccm_total = mem.get('CCMRAM', 0)

    if len(sys.argv) > 2:
        flash_total = parse_size(sys.argv[2])
    if len(sys.argv) > 3:
        ram_total = parse_size(sys.argv[3])
    if len(sys.argv) > 4:
        ccm_total = parse_size(sys.argv[4])

    def show(name, used, total):
        pct = used * 100.0 / total if total else 0
        print('  %-20s  %10s  %10s  %8.2f%%' % (name, fmt_size(used), fmt_size(total), pct))

    print('  %-20s  %10s  %10s  %8s' % ('Memory region', 'Used Size', 'Region Size', '%age Used'))
    print('  %-20s  %10s  %10s  %8s' % ('====================', '==========', '==========', '=========='))
    show('FLASH:', flash_used, flash_total)
    show('RAM:', ram_used, ram_total)
    if ccm_total > 0:
        show('CCMRAM:', ccm_used, ccm_total)
    print()

if __name__ == '__main__':
    main()
