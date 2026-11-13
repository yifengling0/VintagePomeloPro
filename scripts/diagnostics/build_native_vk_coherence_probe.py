#!/usr/bin/env python3
"""Build the bounded x86_64 Vulkan memory probe, without downloading dependencies.

Runs under the Linux build host. The binary is executed separately through the
actual Steam/FEX transport; compiling it is not evidence of GPU correctness.
"""
import argparse, hashlib, json, shutil, struct, subprocess
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--memory-type', type=int, default=0, choices=range(32))
    parser.add_argument('--bytes', type=int, default=65536)
    parser.add_argument('--steam-launch', action='store_true')
    parser.add_argument('--images', action='store_true',
                        help='Also check odd-sized UNORM/sRGB images, mip/layers and suballocation.')
    parser.add_argument('--textures', action='store_true',
                        help='Experimental opaque BC1 -> ETC2 GPU transcode and hardware sampling.')
    parser.add_argument('--bc', action='store_true',help='All-BC decoder/reference and real image resource tests.')
    parser.add_argument('--api11',action='store_true',help='Match Unity Vulkan 1.1 core/extension dispatch.')
    parser.add_argument('--glslang', type=Path, default=Path('/usr/bin/glslangValidator'))
    parser.add_argument('--texture-width', type=int, default=127, choices=range(1,1025))
    parser.add_argument('--texture-height', type=int, default=65, choices=range(1,1025))
    parser.add_argument('--image-rounds', type=int, default=32, choices=range(1, 129))
    args = parser.parse_args()
    if args.bc and (args.textures or args.images):parser.error('--bc is a separate full resource probe')
    if args.bytes < 4 or args.bytes > 64 * 1024 * 1024 or args.bytes % 4:
        parser.error('--bytes must be word-aligned and between 4 and 67108864')
    compiler = Path(shutil.which('gcc') or '/missing-gcc').resolve(strict=True)
    if subprocess.check_output([str(compiler), '-dumpmachine'], text=True).strip() != 'x86_64-linux-gnu':
        raise SystemExit('An x86_64-linux-gnu compiler is required')
    source = Path(__file__).with_name('native_vk_coherence_probe.c')
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    binary = out / 'native_vk_coherence_probe'
    manifest = out / 'native-vk-coherence-build.json'
    inputs = dict(sourceSha256=sha(source), builderSha256=sha(Path(__file__)),compilerSha256=sha(compiler),
                  memoryType=args.memory_type, allocationBytes=args.bytes,
                  requestedApi='1.1' if args.api11 else '1.2',
                  steamArgumentsIgnored=args.steam_launch,
                  vulkanHeaderSha256=sha(Path('/usr/include/vulkan/vulkan_core.h')))
    if args.images:
        inputs.update(imageHeaderSha256=sha(source.with_name('native_vk_image_probe.h')),
                      imageRounds=args.image_rounds)
    if args.bc:
        root=Path(__file__).resolve().parents[2]
        bc_source=root/'entry/src/main/cpp/linux_steam/gnu_fixture/native_bc'
        reference=root/'host_tests/vendor/bcdec'
        subprocess.run(['python3',str(Path(__file__).with_name('build_native_bc_shaders.py')),
            '--output-dir',str(out),'--glslang',str(args.glslang.resolve())],check=True,timeout=120)
        inputs.update(bcHeaderSha256=sha(source.with_name('native_bc_probe.h')),bcdecSha256=sha(reference/'bcdec.h'),
            bcSpirvSha256=sha(out/'native_bc_spirv.h'),sampleSourceSha256=sha(source.with_name('native_bc_sample.comp')))
    shaders = [('bc1_etc2', 'bc1_etc2_probe.comp'), ('etc2_sample', 'etc2_sample_probe.comp')]
    if args.textures:
        inputs.update(textureHeaderSha256=sha(source.with_name('native_texture_probe.h')),
                      glslangSha256=sha(args.glslang.resolve(strict=True)),
                      glslangVersion=subprocess.check_output(
                          [str(args.glslang.resolve()), '--version'], text=True, timeout=15).strip(),
                      textureWidth=args.texture_width, textureHeight=args.texture_height,
                      textureShaders={name: sha(source.with_name(name)) for _, name in shaders})
    if manifest.exists() and binary.exists():
        old = json.loads(manifest.read_text())
        if old.get('inputs') == inputs and old.get('sha256') == sha(binary):
            print('Reused verified Vulkan memory probe')
            return
    if args.textures:
        lines=['/* Generated from the diagnostic GLSL. */']
        for symbol, name in shaders:
            spv = out / (name + '.spv')
            subprocess.run([str(args.glslang), '-V', '--target-env', 'vulkan1.1',
                            '-o', str(spv), str(source.with_name(name))], check=True, timeout=45)
            data=spv.read_bytes()
            if len(data)%4 or len(data)>1024*1024: raise ValueError('Invalid bounded SPIR-V')
            words=struct.unpack('<'+'I'*(len(data)//4), data)
            lines.append('static const uint32_t '+symbol+'_spirv[]={'+','.join(hex(w) for w in words)+'};')
        (out/'texture_probe_spirv.h').write_text('\n'.join(lines)+'\n')
    if args.bc:
        spv=out/'native_bc_sample.spv'
        subprocess.run([str(args.glslang.resolve()),'-V','--target-env','vulkan1.1','-o',str(spv),str(source.with_name('native_bc_sample.comp'))],check=True,timeout=45)
        data=spv.read_bytes();words=struct.unpack('<'+'I'*(len(data)//4),data)
        (out/'native_bc_sample_spirv.h').write_text('static const uint32_t native_bc_sample_spirv[]={'+','.join(hex(w) for w in words)+'};\n')
    command = [str(compiler), '-std=c11', '-O2', '-g0', '-Wall', '-Wextra', '-Werror',
               f'-DPROBE_MEMORY_TYPE={args.memory_type}', f'-DPROBE_BYTES={args.bytes}',
               *(['-DPROBE_STEAM_LAUNCH=1'] if args.steam_launch else []),
               *(['-DPROBE_API11=1'] if args.api11 else []),
               *(['-DPROBE_IMAGES=1', f'-DPROBE_IMAGE_ROUNDS={args.image_rounds}'] if args.images else []),
               *(['-DPROBE_TEXTURES=1', f'-DPROBE_TEXTURE_WIDTH={args.texture_width}',
                  f'-DPROBE_TEXTURE_HEIGHT={args.texture_height}', '-I'+str(out)] if args.textures else []),
               *(['-DPROBE_BC=1','-I'+str(out),'-I'+str(reference)] if args.bc else []),
               str(source), '-Wl,--wrap=__libc_start_main', '-ldl', '-lm', '-o', str(binary)]
    subprocess.run(command, check=True, timeout=45)
    record = dict(inputs=inputs, command=command, sha256=sha(binary), bytes=binary.stat().st_size,
                  architecture='x86_64-linux-gnu', deviceExecuted=False, gpuExecuted=False)
    if args.textures:
        record['spirvSha256'] = {name: sha(out/(name+'.spv')) for _, name in shaders}
    manifest.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({key: record[key] for key in ['sha256', 'bytes', 'architecture']}))


if __name__ == '__main__':
    main()
