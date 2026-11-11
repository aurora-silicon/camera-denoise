#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the Neural Engine low-light program for one grid size.

The network works on the 2x2 space-to-depth grid of the luma plane (plane c
holds pixel (2y + c/2, 2x + c%2)): 1x1 4->32, depthwise 3x3, 1x1 32->32,
3x3 32->32, depthwise 3x3, 1x1 32->16, ReLU after all but the last, SAME
padding, bias everywhere. Output planes 0-3 are a residual in [0, 1] luma
units; planes 4-15 have zero weights.

usage: build-model.py --weights data/ane/weights.bin --mil-hwxc PATH
                      --sections PATH/h16-sections.py --width 960 --height 540
                      --out data/ane/h16g-960x540

mil-hwxc is the ANE compiler from mil-hwx-compiler. h16-sections.py builds the
four H16 load sections from the HWX (Linux tools/testing/selftests/drivers/ane).
"""
import argparse
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

LAYERS = [  # cin, cout, kernel, groups, relu
    (4, 32, 1, 1, True), (32, 32, 3, 32, True), (32, 32, 1, 1, True),
    (32, 32, 3, 1, True), (32, 32, 3, 32, True), (32, 16, 1, 1, False)]


def split_weights(data):
    out, at = [], 0
    for ci, co, k, g, _ in LAYERS:
        n = co * (ci // g) * k * k * 2
        out.append((data[at:at + n], data[at + n:at + n + co * 2]))
        at += n + co * 2
    if at != len(data):
        raise SystemExit(f'weights: {len(data)} bytes, the network needs {at}')
    return out


def blobfile(parts):
    """BLOBFILE v2: a 64-byte header, then a 64-byte record and the 64-aligned payload per constant."""
    out = bytearray(struct.pack('<II', 1, 2) + bytes(56))
    offsets = []
    for payload in parts:
        meta = len(out)
        out += struct.pack('<IIQQ', 0xDEADBEEF, 1, len(payload), meta + 64) + bytes(40)
        out += payload + bytes((-len(payload)) % 64)
        offsets.append(meta)
    return bytes(out), offsets


def mil(width, height, offsets):
    ref = 'BLOBFILE(path = string("@model_path/weights/weight.bin"), offset = uint64({}))'
    body, cur = [], 'x'
    for i, (ci, co, k, g, relu) in enumerate(LAYERS):
        shape = f'[1, {co}, {height}, {width}]'
        body += [
            f'    string pt{i} = const()[name = string("pt{i}"), val = string("same")];',
            f'    tensor<int32, [2]> st{i} = const()[name = string("st{i}"), val = tensor<int32, [2]>([1, 1])];',
            f'    tensor<int32, [4]> pd{i} = const()[name = string("pd{i}"), val = tensor<int32, [4]>([0, 0, 0, 0])];',
            f'    tensor<int32, [2]> dl{i} = const()[name = string("dl{i}"), val = tensor<int32, [2]>([1, 1])];',
            f'    int32 gp{i} = const()[name = string("gp{i}"), val = int32({g})];',
            f'    tensor<fp16, [{co}, {ci // g}, {k}, {k}]> W{i} = const()[name = string("W{i}"), '
            f'val = tensor<fp16, [{co}, {ci // g}, {k}, {k}]>({ref.format(offsets[2 * i])})];',
            f'    tensor<fp16, [{co}]> B{i} = const()[name = string("B{i}"), '
            f'val = tensor<fp16, [{co}]>({ref.format(offsets[2 * i + 1])})];',
            f'    tensor<fp16, {shape}> c{i} = conv(bias = B{i}, dilations = dl{i}, groups = gp{i}, '
            f'pad = pd{i}, pad_type = pt{i}, strides = st{i}, weight = W{i}, x = {cur})[name = string("conv{i}")];',
        ]
        cur = f'c{i}'
        if relu:
            body.append(f'    tensor<fp16, {shape}> r{i} = relu(x = {cur})[name = string("relu{i}")];')
            cur = f'r{i}'
    return ('program(1.3)\n[buildInfo = dict<string, string>({})]\n{\n'
            f'  func main<ios18>(tensor<fp16, [1, 4, {height}, {width}]> x) {{\n' + '\n'.join(body) +
            f'\n  }} -> ({cur});\n}}\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--weights', type=pathlib.Path, required=True)
    ap.add_argument('--mil-hwxc', required=True)
    ap.add_argument('--sections', required=True)
    ap.add_argument('--width', type=int, required=True)
    ap.add_argument('--height', type=int, required=True)
    ap.add_argument('--out', type=pathlib.Path, required=True)
    a = ap.parse_args()
    parts = [p for pair in split_weights(a.weights.read_bytes()) for p in pair]
    data, offsets = blobfile(parts)
    with tempfile.TemporaryDirectory() as tmp:
        src = pathlib.Path(tmp) / 'src'
        (src / 'weights').mkdir(parents=True)
        (src / 'weights' / 'weight.bin').write_bytes(data)
        (src / 'model.mil').write_text(mil(a.width, a.height, offsets))
        subprocess.run([a.mil_hwxc, '--target', 'H16G', '--format', 'hwx', '--schedule', 'linked',
                        '--mil', str(src / 'model.mil'), '--model-root', str(src),
                        '--output', str(pathlib.Path(tmp) / 'out')], check=True)
        objects = sorted((pathlib.Path(tmp) / 'out').rglob('*.hwx'))
        if len(objects) != 1:
            raise SystemExit(f'expected one program, got {len(objects)}')
        a.out.mkdir(parents=True, exist_ok=True)
        shutil.copy(objects[0], a.out / 'model.hwx')
    subprocess.run([sys.executable, a.sections, '--hwx', str(a.out / 'model.hwx'), '--out-dir', str(a.out)],
                   check=True)
    print(f'{a.out}: model.hwx and the four load sections')


if __name__ == '__main__':
    main()
