#!/usr/bin/env python3
"""Build the Windows proxy with the tracked Linux MinGW cross toolchain.

Run from any directory. Does not download dependencies or install into a game.
Use --dry-run to inspect commands without requiring installed build tools.
"""
import argparse
from pathlib import Path
import shlex
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def build(args):
    native = args.native_root.resolve()
    sdl = (args.sdl_include or native / '_work/deps/linux/prefix/include').resolve()
    output = args.build.resolve()
    required = [native / 'public/include/remix/remix.h',
                native / 'include/vulkan/include/vulkan/vulkan.h',
                sdl / 'SDL3/SDL.h']
    for path in required:
        if not path.is_file():
            raise ValueError(f'Missing header: {path}. See the fresh-clone setup guide.')
    if output == ROOT or output in ROOT.parents:
        raise ValueError('Build directory must not be the source root or its ancestor.')
    setup = ['meson', 'setup', str(output), str(ROOT),
             '--cross-file', str(ROOT / 'cross/x86_64-w64-mingw32.txt'),
             '-Dbuildtype=release', f'-Dopenremix_include={native / "public/include"}',
             f'-Dsdl_include={sdl}']
    if (output / 'meson-private/coredata.dat').is_file():
        setup.append('--reconfigure')
    commands = [setup, ['meson', 'compile', '-C', str(output), '-j', str(args.jobs)]]
    if not args.dry_run:
        if not sys.platform.startswith('linux'):
            raise ValueError('This helper requires Linux for the tracked MinGW cross build.')
        for tool in ['meson', 'ninja', 'glslangValidator', 'x86_64-w64-mingw32-gcc',
                     'x86_64-w64-mingw32-g++', 'x86_64-w64-mingw32-ar',
                     'x86_64-w64-mingw32-strip', 'x86_64-w64-mingw32-windres']:
            if not shutil.which(tool):
                raise ValueError(f'Missing tool on PATH: {tool}')
    for command in commands:
        print(shlex.join(command), flush=True)
        if not args.dry_run:
            subprocess.run(command, cwd=ROOT, check=True)
    if not args.dry_run:
        print(f'Proxy: {output / "shaderapidx9.dll"}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-root', type=Path, default=ROOT.parent / 'OpenRemix',
                        help='OpenRemix clone (default: sibling OpenRemix directory)')
    parser.add_argument('--sdl-include', type=Path,
                        help='SDL include root containing SDL3/SDL.h; defaults to native Linux dependency prefix')
    parser.add_argument('--build', type=Path, default=ROOT / '_work/build/mingw',
                        help='Build output directory (default: adapter _work/build/mingw)')
    parser.add_argument('--jobs', type=int, default=4, help='Parallel compile jobs (default: 4)')
    parser.add_argument('--dry-run', action='store_true',
                        help='Validate headers and print commands without executing them')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    try:
        build(args)
    except (ValueError, OSError) as error:
        parser.exit(1, f'{error}\n')
    except subprocess.CalledProcessError as error:
        return error.returncode
    return 0


if __name__ == '__main__':
    sys.exit(main())
