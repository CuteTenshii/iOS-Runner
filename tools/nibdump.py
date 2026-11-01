#!/usr/bin/env python3
"""Prints the objects in a compiled .nib: each class with its keys and values.

usage: tools/nibdump.py FILE.nib

Run it with `python3 -I` when the nib came from an untrusted app.
"""
import struct
import sys

HEADER_COUNTS_OFFSET = 10
FIXED_VALUE_FORMATS = {0: '<B', 1: '<H', 2: '<i', 3: '<q', 6: '<f', 7: '<d'}
TYPE_TRUE, TYPE_FALSE, TYPE_DATA, TYPE_NIL, TYPE_OBJECT = 4, 5, 8, 9, 10


def varint(data, at):
    value = shift = 0
    while True:
        byte = data[at]
        at += 1
        value |= (byte & 0x7f) << shift
        shift += 7
        if byte & 0x80:
            return value, at


def read_value(data, at):
    kind = data[at]
    at += 1
    if kind in FIXED_VALUE_FORMATS:
        layout = FIXED_VALUE_FORMATS[kind]
        return struct.unpack_from(layout, data, at)[0], at + struct.calcsize(layout)
    if kind == TYPE_TRUE:
        return True, at
    if kind == TYPE_FALSE:
        return False, at
    if kind == TYPE_DATA:
        length, at = varint(data, at)
        return data[at:at + length], at + length
    if kind == TYPE_NIL:
        return None, at
    if kind == TYPE_OBJECT:
        return ('obj', struct.unpack_from('<I', data, at)[0]), at + 4
    raise ValueError(f'unknown nib value type {kind}')


def load(path):
    with open(path, 'rb') as file:
        data = file.read()
    _, _, object_count, objects_at, key_count, keys_at, value_count, values_at, class_count, classes_at = \
        struct.unpack_from('<10I', data, HEADER_COUNTS_OFFSET)
    keys, at = [], keys_at
    for _ in range(key_count):
        length, at = varint(data, at)
        keys.append(data[at:at + length].decode())
        at += length
    values, at = [], values_at
    for _ in range(value_count):
        key, at = varint(data, at)
        value, at = read_value(data, at)
        values.append((keys[key], value))
    classes, at = [], classes_at
    for _ in range(class_count):
        length, at = varint(data, at)
        extras, at = varint(data, at)
        at += 4 * extras
        classes.append(data[at:at + length - 1].decode())
        at += length
    objects, at = [], objects_at
    for _ in range(object_count):
        cls, at = varint(data, at)
        start, at = varint(data, at)
        count, at = varint(data, at)
        objects.append((classes[cls], values[start:start + count]))
    return objects


def printable(value):
    if not isinstance(value, bytes):
        return value
    try:
        return value.decode()
    except UnicodeDecodeError:
        return value.hex()[:48]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    for index, (cls, values) in enumerate(load(sys.argv[1])):
        print(index, cls)
        for key, value in values:
            print('    ', key, '=', printable(value))


if __name__ == '__main__':
    main()
