#!/usr/bin/env python3
"""Reads a guest's Mach-O binary: what its code calls, and the values its references point at.

usage:
  tools/macho.py dis BINARY START END        disassemble an address range, with selector names filled in
  tools/macho.py calls BINARY START END      only the calls, selectors and string literals in the range
  tools/macho.py selector BINARY ADDRESS...  the selector a selref slot holds
  tools/macho.py typeref BINARY ADDRESS...   the mangled name behind a demangling cache's relative reference

Addresses are as llvm-objdump prints them, in hex. A crash's app+0xOFFSET is 0x100000000 + OFFSET.
Run it with `python3 -I` when the binary came from an untrusted app.
"""
import re
import struct
import subprocess
import sys

CHAINED_TARGET_MASK = 0xfffffffff
BASE_ADDRESS = 0x100000000
SEGMENT_COMMAND = 0x19


class Binary:
    def __init__(self, path):
        with open(path, 'rb') as file:
            self.data = file.read()
        self.segments = []
        count = struct.unpack_from('<I', self.data, 16)[0]
        at = 32
        for _ in range(count):
            command, size = struct.unpack_from('<II', self.data, at)
            if command == SEGMENT_COMMAND:
                address, length, offset = struct.unpack_from('<QQQ', self.data, at + 24)
                self.segments.append((address, length, offset))
            at += size

    def offset(self, address):
        for start, length, offset in self.segments:
            if start <= address < start + length:
                return address - start + offset
        return None

    def c_string(self, address):
        at = self.offset(address)
        if at is None:
            return None
        end = self.data.find(b'\0', at)
        return self.data[at:end].decode('latin-1')

    def selector(self, slot):
        at = self.offset(slot)
        if at is None:
            return None
        value = struct.unpack_from('<Q', self.data, at)[0]
        return self.c_string(BASE_ADDRESS + (value & CHAINED_TARGET_MASK))

    def typeref(self, address):
        at = self.offset(address)
        relative, length = struct.unpack_from('<ii', self.data, at)
        name = self.offset(address + relative)
        return self.data[name:name + length]


def disassemble(path, start, end):
    """Yields the instructions in a range. llvm-objdump ignores its address options with --macho, which names the stubs."""
    process = subprocess.Popen(['llvm-objdump', '--macho', '-d', path], stdout=subprocess.PIPE, text=True, errors='replace')
    try:
        for line in process.stdout:
            match = re.match(r'\s*([0-9a-f]{9,}):', line)
            if not match:
                continue
            address = int(match.group(1), 16)
            if address > end:
                break
            if address >= start:
                yield line.rstrip('\n')
    finally:
        process.kill()
        process.wait()


def annotated(binary, lines):
    """Yields (address, instruction, selector), following each adrp into the ldr x1 that loads a selector."""
    pages = {}
    for line in lines:
        fields = line.split('\t')
        address = fields[0].strip().rstrip(':')
        instruction = ' '.join(fields[2:]).strip()
        selector = None
        page = re.match(r'adrp\s+(x\d+), .*; (0x[0-9a-f]+)', instruction)
        if page:
            pages[page.group(1)] = int(page.group(2), 16)
        load = re.match(r'ldr\s+x1, \[(x\d+), #(0x[0-9a-f]+)\]', instruction)
        if load and load.group(1) in pages:
            selector = binary.selector(pages[load.group(1)] + int(load.group(2), 16))
            if selector is not None and not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_:]*', selector):
                selector = None
        yield address, instruction, selector


def main(arguments):
    if len(arguments) < 3:
        sys.exit(__doc__)
    command, path, *values = arguments
    binary = Binary(path)
    numbers = [int(value, 16) for value in values]
    if command == 'selector':
        for number in numbers:
            print(f'{number:#x} {binary.selector(number)}')
    elif command == 'typeref':
        for number in numbers:
            print(f'{number:#x} {binary.typeref(number)!r}')
    elif command in ('dis', 'calls'):
        for address, instruction, selector in annotated(binary, disassemble(path, numbers[0], numbers[1])):
            if command == 'dis':
                print(f'{address} {instruction}' + (f'  ; {selector}' if selector else ''))
            elif selector:
                print(f'{address} selector {selector}')
            elif 'symbol stub for' in instruction or 'literal pool for' in instruction:
                print(f'{address} ' + instruction.split('; ')[-1].replace('symbol stub for: ', '').replace('literal pool for: ', ''))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
