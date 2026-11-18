#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Compile and optimize a shader at build time, preserving Vulkan semantics."""
import argparse
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile

OPTIMIZER_OPTIONS = ['-O', '--scalar-block-layout']


def check_converted_sampler_indices(words):
    """Gamescope binding 0:4 uses immutable YCbCr conversion samplers.

    This is a focused build contract, not general SPIR-V validation. Require
    literal indices after the layer loops have been optimized and unrolled.
    """
    constants, bindings, sets, instructions = set(), {}, {}, []
    offset = 5
    while offset < len(words):
        size, opcode = words[offset] >> 16, words[offset] & 0xffff
        if not size or offset + size > len(words):
            raise ValueError('Malformed SPIR-V instruction')
        operands = words[offset+1:offset+size]
        instructions.append((opcode, operands))
        if opcode == 43 and len(operands) >= 3:  # OpConstant
            constants.add(operands[1])
        elif opcode == 71 and len(operands) == 3:  # OpDecorate
            if operands[1] == 33:
                bindings[operands[0]] = operands[2]
            elif operands[1] == 34:
                sets[operands[0]] = operands[2]
        offset += size
    converted = {variable for variable, binding in bindings.items()
                 if binding == 4 and sets.get(variable) == 0}
    accesses = 0
    for opcode, operands in instructions:
        if opcode in (65, 66) and len(operands) >= 4 and operands[2] in converted:
            if operands[3] not in constants:
                raise ValueError('YCbCr sampler descriptor index is not a literal constant; layer unroll failed')
            accesses += 1
    return accesses


def publish(path, data):
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(data)
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def compile_shader(compiler, optimizer, source, output, name, includes=(), depfile=None, spirv_output=None):
    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', name):
        raise ValueError('Invalid C shader identifier')
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.hsl-shader-', dir=output.parent) as directory:
        work = Path(directory)
        raw, optimized, dependencies = work/'raw.spv', work/'optimized.spv', work/'shader.d'
        command = [str(compiler), '-V', str(source), '-o', str(raw)]
        command += ['-I'+str(path) for path in includes]
        if depfile:
            command += ['--depfile', str(dependencies)]
        subprocess.run(command, check=True, timeout=120)
        # No specialization freezing, relaxed math or capability changes.
        subprocess.run([str(optimizer), *OPTIMIZER_OPTIONS, str(raw), '-o', str(optimized)],
                       check=True, timeout=120)
        data = optimized.read_bytes()
        if len(data) < 20 or len(data) > 32*1024*1024 or len(data) % 4:
            raise ValueError('Invalid SPIR-V size')
        words = struct.unpack('<'+'I'*(len(data)//4), data)
        if words[0] != 0x07230203:
            raise ValueError('Invalid SPIR-V magic')
        converted_accesses = check_converted_sampler_indices(words)
        header = ('// Generated from GLSL by glslang and SPIRV-Tools.\n#pragma once\n'
                  'static const uint32_t '+name+'[] = {\n'+
                  ','.join(hex(word) for word in words)+'\n};\n').encode()
        if depfile:
            # glslang targets its raw temporary output. Ninja must track the
            # final header, and all original source/include prerequisites.
            _, separator, prerequisites = dependencies.read_text().partition(': ')
            if not separator:
                raise ValueError('Malformed shader dependency file')
            target = str(output).replace('\\', '\\\\').replace(' ', '\\ ').replace('#', '\\#').replace('$', '$$')
            publish(Path(depfile), (target+': '+prerequisites).encode())
        if spirv_output:
            publish(Path(spirv_output), data)
        publish(output, header)
        return {'unoptimizedBytes': raw.stat().st_size, 'optimizedBytes': len(data),
                'spirvVersion': words[1], 'runtimeCompilation': False,
                'constantYcbcrAccesses': converted_accesses}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('compiler', 'optimizer', 'source', 'output', 'name'):
        parser.add_argument('--'+option, required=True)
    parser.add_argument('--depfile')
    args = parser.parse_args()
    compile_shader(args.compiler, args.optimizer, args.source, args.output, args.name, depfile=args.depfile)
