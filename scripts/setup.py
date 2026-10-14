#!/usr/bin/env python3
"""One entry point: from a fresh clone and your own dump to a runnable native build of the game.

    python scripts/setup.py --dump <folder> [--jobs N] [tool locations ...]
    python scripts/setup.py --target linux-arm64 --dump <folder> [--sysroot <dir>] [--llvm <dir>]     Steam Frame (aarch64 Linux), see below

<folder> holds what you dumped from YOUR console (see BUILDING.md):
    <folder>/exefs/    the title's ExeFS files: main  sdk  (rtld, subsdk0, main.npdm are not used), update applied
    <folder>/romfs/    the title's RomFS (data.arc ...), update applied
    optional: <folder>/sysdata/fonts (the system's shared fonts), <folder>/sysdata/zoneinfo

Nothing of the game is stored in the repository: every step below derives its files from the dump, into <repo>/build
(ignored by git). Steps run in this order and are resumable (a finished step is skipped; --redo <step> repeats it,
--from <step> repeats it and everything after it; the compilers themselves only rebuild what changed):

    deps      check the tools and the dump                    stage    lay the sources out in build/work
    convert   dump -> main.elf / sdk.elf (tools/extract)      modules  game's script modules out of data.arc
    tables    every generated header and table                lift     lift main (recomp_eh.py)
    sdk       lift the system-library functions (sdklift.py)  compile  main game library (clang, -O1)
    nro       lift + compile the script modules               link     host runtime + game library -> exe
    check     self-tests that need no game run

--target linux-arm64 builds for the Steam Frame (default: windows). Two modes, detected from the machine:
    cross   x86-64 Linux/WSL: clang + lld from an LLVM release (--llvm / LLVM_ROOT / ~/.local/llvm-*), headers and libraries from a
            sysroot copied from the device (toolchain/sync_sysroot.sh; --sysroot / SYSROOT, default ~/sysroots/steam-frame)
    native  on the Frame itself (aarch64): system clang/lld and /usr; SDL3 from ~/ssbu-port/deps/sdl3 (or --sdl3)
  The build folder is <repo>/build/linux-arm64 (the windows one is untouched). `deps` also compiles, links and (native, or cross with
  SSBU_DEVICE=steamos@frame) runs toolchain/hello_sdl3_vulkan.c. Output in build/linux-arm64/work: ssbu_native, libssbu_game_eh_o1.so,
  libssbu_glslang.so, libSDL3.so.0, nro_bin_eh_o2/*.so.  Details: BUILDING.md (Linux / Steam Frame), toolchain/README.md.

Other commands:  --status (what is done)   --steps (list)   --only <step>   --run (print the command that starts the game)
Environment variables named RECOMP_* / SSBU_* are removed from the children's environment; tool locations come from the
options below, from LLVM_ROOT / SDL3_ROOT / VULKAN_HEADERS / GLSLANG_ROOT / SUYU_ROOT, or from <repo>/deps/<name>.
"""
import argparse
import glob
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STEPS = ['deps', 'stage', 'convert', 'modules', 'tables', 'lift', 'sdk', 'compile', 'nro', 'link', 'check']
GAME_TEXT_SIZE, SDK_TEXT_SIZE = 0x39c7e90, 0x540950          # 13.0.4: the lifter's constants (recomp.py, sdklift.py)
EXE = 'ssbu_native.exe'                 # windows; Ctx.exe has the name for the selected target
DEFAULT_DEVICE_DIR = '~/ssbu-port/build/deps_probe'      # where the cross `deps` probe is run on the device


# ---------------------------------------------------------------------------------------------- helpers
class Fail(Exception):
    pass


def say(msg=''):
    print(msg.encode('ascii', 'replace').decode('ascii'), flush=True)


def fmt(secs):
    return '%dm%02ds' % divmod(int(secs), 60) if secs >= 60 else '%.1fs' % secs


def disk_mb(path):
    total = 0
    for root, _, files in os.walk(path):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    return total / 1048576.0


class Ctx:
    def __init__(self, a):
        self.a = a
        self.state = {}
        pre = os.path.abspath(a.build) if a.build else None
        self.target = a.target or 'windows'
        self.linux = self.target != 'windows'
        self.native = self.linux and platform.machine() in ('aarch64', 'arm64') and sys.platform.startswith('linux')
        self.exe = 'ssbu_native' if self.linux else EXE
        self.so = '.so' if self.linux else '.dll'
        self.game_lib = ('libssbu_game_eh_o1.so') if self.linux else 'ssbu_game_eh_o1.dll'
        self.target_flags = []                   # linux: --target/--sysroot/-march ... (set by deps)
        self.build = pre or os.path.join(REPO, os.path.join('build', 'linux-arm64') if self.linux else 'build')
        self.work = os.path.join(self.build, 'work')
        self.bin = os.path.join(self.build, 'binaries', '13.0.4')
        self.logs = os.path.join(self.build, 'logs')
        self.state_file = os.path.join(self.build, 'state.json')
        if os.path.exists(self.state_file):
            with open(self.state_file) as f:
                self.state = json.load(f)
        if self.linux:
            self.target_flags = self.state.get('target_flags', [])
        self.jobs = a.jobs or min(24, os.cpu_count() or 8)
        self.tools = dict(self.state.get('tools', {})) if self.linux else {}      # located tools, filled by deps (linux: remembered, so --only <step> works)

    def save(self):
        os.makedirs(self.build, exist_ok=True)
        with open(self.state_file, 'w') as f:
            json.dump(self.state, f, indent=1)

    def env(self, **extra):
        env = {k: v for k, v in os.environ.items() if not k.startswith(('RECOMP_', 'SSBU_'))}
        env['PYTHONIOENCODING'] = 'utf-8'
        env['PYTHONUNBUFFERED'] = '1'
        t = self.tools
        if 'clang' in t:
            env['RECOMP_CLANG'] = t['clang']
        if self.linux:
            env['RECOMP_TARGET'] = self.target
            env['RECOMP_TARGET_FLAGS'] = ' '.join(shlex.quote(f) for f in self.target_flags)
            compat = os.path.expanduser('~/.local/llvm-9-compat')      # libtinfo.so.5 stub for the LLVM 9.0.0 tarball on newer distributions
            if '/llvm-9' in t.get('clang', '') and os.path.isdir(compat):
                env['LD_LIBRARY_PATH'] = compat + (os.pathsep + env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '')
        if 'sdl3' in t:
            env['SDL3_ROOT'] = t['sdl3']
        if 'vulkan' in t:
            env['VULKAN_HEADERS'] = t['vulkan']
        if 'glslang' in t:
            env['GLSLANG_ROOT'] = t['glslang']
        if 'suyu' in t:
            env['SUYU_ROOT'] = t['suyu']
        env.update(extra)
        return env

    def run(self, step, argv, cwd=None, env=None, log=None, ok=(0,)):
        """Run a child, its output to logs/<log or step>.log; returns the output text. Raises Fail on a bad exit code."""
        os.makedirs(self.logs, exist_ok=True)
        path = os.path.join(self.logs, (log or step) + '.log')
        t0 = time.time()
        with open(path, 'ab') as f:
            f.write(('\n$ %s\n' % ' '.join(argv)).encode())
            p = subprocess.run(argv, cwd=cwd or self.work, env=env or self.env(), stdout=f, stderr=subprocess.STDOUT)
        with open(path, 'rb') as f:
            f.seek(max(0, os.path.getsize(path) - 6000))
            tail = f.read().decode('utf-8', 'replace')
        if p.returncode not in ok:
            raise Fail('%s failed (exit %s) after %s; see %s\n%s' % (' '.join(argv[:3]), p.returncode, fmt(time.time() - t0), path, tail[-1500:]))
        return tail


def find_first(cands, probe):
    for c in cands:
        if c and os.path.isdir(c) and probe(c):
            return os.path.abspath(c)
    return None


# ---------------------------------------------------------------------------------------------- steps
def deps_windows(cx):
    a = cx.a
    deps = os.path.join(REPO, 'deps')
    llvm = find_first([a.llvm, os.environ.get('LLVM_ROOT'), r'C:\llvm-9.0.0', r'C:\Program Files\LLVM', os.path.join(deps, 'llvm')],
                      lambda d: os.path.exists(os.path.join(d, 'bin', 'clang.exe')))
    if not llvm:
        raise Fail('LLVM/clang 9.0.0 not found (--llvm <folder with bin\\clang.exe>)')
    clang = os.path.join(llvm, 'bin', 'clang.exe')
    ver = subprocess.run([clang, '--version'], capture_output=True, text=True).stdout.splitlines()[0]
    say('  clang: %s   [%s]' % (ver, clang))
    if 'version 9.' not in ver and 'version 8.' not in ver:
        say('  WARNING: the lifted code was only ever compiled with clang 8 and 9; %s is untested' % ver)
    cx.tools['clang'] = clang
    probe = os.path.join(cx.build, 'probe')
    os.makedirs(probe, exist_ok=True)
    with open(os.path.join(probe, 'p.c'), 'w') as f:
        f.write('#include <windows.h>\n#include <stdio.h>\nint main(void){puts("ok");return 0;}\n')
    r = subprocess.run([clang, os.path.join(probe, 'p.c'), '-o', os.path.join(probe, 'p.exe'), '-luser32'], capture_output=True, text=True)
    if r.returncode or subprocess.run([os.path.join(probe, 'p.exe')], capture_output=True, text=True).stdout.strip() != 'ok':
        raise Fail('clang cannot compile and link a Windows program: install the MSVC Build Tools (C++ x64 tools) and a Windows 10/11 SDK\n' + r.stderr[-800:])
    say('  clang + MSVC linker + Windows SDK: ok')

    for key, opt, envv, sub, probe_rel in (
            ('sdl3', a.sdl3, 'SDL3_ROOT', 'SDL3', os.path.join('include', 'SDL3', 'SDL.h')),
            ('vulkan', a.vulkan_headers, 'VULKAN_HEADERS', 'Vulkan-Headers', os.path.join('include', 'vulkan', 'vulkan.h')),
            ('glslang', a.glslang, 'GLSLANG_ROOT', 'glslang', os.path.join('include', 'glslang', 'Include', 'glslang_c_interface.h')),
            ('suyu', a.suyu, 'SUYU_ROOT', 'suyu', os.path.join('src', 'core', 'hle', 'service', 'mii', 'types', 'raw_data.cpp'))):
        found = find_first([opt, os.environ.get(envv), os.path.join(deps, sub)], lambda d, p=probe_rel: os.path.exists(os.path.join(d, p)))
        if not found:
            raise Fail('%s not found (--%s <folder containing %s>)' % (sub, {'sdl3': 'sdl3', 'vulkan': 'vulkan-headers', 'glslang': 'glslang', 'suyu': 'suyu'}[key], probe_rel))
        cx.tools[key] = found
        say('  %-8s %s' % (sub, found))
    if not os.path.exists(os.path.join(cx.tools['sdl3'], 'lib', 'x64', 'SDL3.lib')):
        raise Fail('SDL3 folder lacks lib\\x64\\SDL3.lib (use the "VC" development package)')
    if not os.path.exists(os.path.join(cx.tools['glslang'], 'lib', 'glslang.lib')):
        raise Fail('glslang folder lacks lib\\glslang.lib (use the windows-x86_64 release package)')
    vsw = os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'), 'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
    vs = subprocess.run([vsw, '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
                        capture_output=True, text=True).stdout.strip() if os.path.exists(vsw) else ''
    if not vs:
        raise Fail('Visual Studio / Build Tools with "MSVC v14x x64/x86 build tools" not found (needed for the glslang shim)')
    say('  MSVC (glslang shim): %s' % vs)


def _llvm_version(clang, env):
    r = subprocess.run([clang, '--version'], capture_output=True, text=True, env=env)
    return (r.stdout.splitlines() or [r.stderr.strip()])[0]


def deps_linux(cx, strict):
    """Probe the linux-arm64 toolchain: compiler + lld, sysroot (cross) or system headers/libs (native), SDL3, Vulkan and glslang
    headers; compile, link and, where possible, run toolchain/hello_sdl3_vulkan.c and compile the glslang shim."""
    a = cx.a
    deps = os.path.join(REPO, 'deps')
    mode = 'native (on the device)' if cx.native else 'cross (x86-64 host -> aarch64 sysroot)'
    say('  target linux-arm64, %s, host %s' % (mode, platform.machine()))
    env = cx.env()
    # ---- compiler
    clang = None
    roots = [a.llvm, os.environ.get('LLVM_ROOT')]
    if not cx.native:
        found = [d for d in glob.glob(os.path.expanduser('~/.local/llvm-*')) + glob.glob(os.path.join(deps, 'llvm*'))
                 if os.path.isfile(os.path.join(d, 'bin', 'clang'))]
        # newest release first; the LLVM 9 tarball (it needs a libtinfo.so.5 stub on current distributions) only when asked for
        found.sort(key=lambda d: (os.path.basename(d).startswith('llvm-9'), [-int(x) for x in re.findall(r'\d+', os.path.basename(d))]))
        roots += found
    for r in roots:
        if r and os.path.isfile(os.path.join(r, 'bin', 'clang')):
            clang = os.path.join(r, 'bin', 'clang')
            break
    if not clang and cx.native:
        clang = shutil.which('clang')
    if not clang:
        raise Fail('clang not found (--llvm <LLVM folder with bin/clang>, LLVM_ROOT, or ~/.local/llvm-<ver>; see toolchain/README.md)')
    cx.tools['clang'] = clang
    env = cx.env()
    ver = _llvm_version(clang, env)
    say('  clang: %s   [%s]' % (ver, clang))
    if 'version 9.' not in ver and 'version 8.' not in ver:
        say('  note: the lifted code was validated with clang 9 (x86-64); %s targets aarch64 here, so re-run the differential test once a dump exists' % ver.split('(')[0].strip())
    lld = os.path.join(os.path.dirname(clang), 'ld.lld')
    if not os.path.exists(lld):
        lld = shutil.which('ld.lld')
    if not lld:
        raise Fail('ld.lld not found next to clang or on PATH (the LLVM release tarballs include it)')
    say('  lld:   %s' % lld)

    # ---- sysroot / system root
    flags = ['--target=aarch64-linux-gnu', '-fuse-ld=lld', '-march=armv8.2-a+lse']
    if cx.native:
        sysroot = None
        if not os.path.exists('/usr/lib/crt1.o') and not glob.glob('/usr/lib/*/crt1.o'):
            raise Fail('no C runtime start files (crt1.o) under /usr/lib: install the C library development files')
        prefix = '/usr'
    else:
        sysroot = os.path.abspath(os.path.expanduser(a.sysroot or os.environ.get('SYSROOT') or '~/sysroots/steam-frame'))
        need = [os.path.join('usr', 'lib', 'crt1.o'), os.path.join('usr', 'lib', 'libc.so'), os.path.join('usr', 'include', 'stdio.h')]
        miss = [n for n in need if not os.path.exists(os.path.join(sysroot, n))]
        if miss:
            raise Fail('sysroot %s is incomplete (%s missing): run toolchain/sync_sysroot.sh -d %s' % (sysroot, ', '.join(miss), sysroot))
        flags.insert(1, '--sysroot=' + sysroot)
        prefix = os.path.join(sysroot, 'usr')
        say('  sysroot: %s' % sysroot)
    cx.target_flags = flags

    # ---- libraries and headers
    dev_sdl = os.path.expanduser('~/ssbu-port/deps/sdl3')
    cands = {
        'sdl3': (a.sdl3, 'SDL3_ROOT', [os.path.join(deps, 'sdl3'), os.path.join(deps, 'SDL3'), dev_sdl] + ([os.path.join(sysroot, 'opt', 'ssbu-deps', 'sdl3')] if sysroot else []),
                 os.path.join('include', 'SDL3', 'SDL.h'), 'SDL3'),
        'vulkan': (a.vulkan_headers, 'VULKAN_HEADERS', [os.path.join(deps, 'Vulkan-Headers'), prefix],
                   os.path.join('include', 'vulkan', 'vulkan.h'), 'Vulkan-Headers'),
        'glslang': (a.glslang, 'GLSLANG_ROOT', [os.path.join(deps, 'glslang'), prefix],
                    os.path.join('include', 'glslang', 'Include', 'glslang_c_interface.h'), 'glslang'),
    }
    for key, (opt, envv, extra, probe_rel, label) in cands.items():
        found = find_first([opt, os.environ.get(envv)] + extra, lambda d, p=probe_rel: os.path.exists(os.path.join(d, p)))
        if not found:
            raise Fail('%s not found (--%s <folder containing %s>)' % (label, {'sdl3': 'sdl3', 'vulkan': 'vulkan-headers', 'glslang': 'glslang'}[key], probe_rel))
        cx.tools[key] = found
        say('  %-8s %s' % (label, found))
    sdl_lib = cx.tools['sdl3']
    if not any(os.path.exists(os.path.join(sdl_lib, 'lib', f)) for f in ('libSDL3.so', 'libSDL3.so.0')):
        raise Fail('SDL3 folder %s lacks lib/libSDL3.so (build it: toolchain/build_deps_on_device.sh, or cmake -DSDL_SHARED=ON)' % sdl_lib)
    gl_lib = os.path.join(cx.tools['glslang'], 'lib')
    gl_names = [f for f in ('libglslang.a', 'libglslang.so') if os.path.exists(os.path.join(gl_lib, f))]
    if not gl_names:
        raise Fail('glslang libraries (libglslang.a or libglslang.so) not found in %s' % gl_lib)
    spv = [f for f in ('libSPIRV-Tools-opt.a', 'libSPIRV-Tools-opt.so') if os.path.exists(os.path.join(gl_lib, f))]
    say('  glslang libs: %s;  SPIRV-Tools-opt: %s' % (', '.join(gl_names), (spv or ['MISSING'])[0]))
    if not spv:
        raise Fail('libSPIRV-Tools-opt (.a or .so) not found in %s' % gl_lib)
    if not a.suyu and not os.environ.get('SUYU_ROOT') and not os.path.isdir(os.path.join(deps, 'suyu')) and not strict:
        say('  suyu:    not given (needed by step tables; only checked with --dump)')
    else:
        found = find_first([a.suyu, os.environ.get('SUYU_ROOT'), os.path.join(deps, 'suyu')],
                           lambda d: os.path.exists(os.path.join(d, 'src', 'core', 'hle', 'service', 'mii', 'types', 'raw_data.cpp')))
        if found:
            cx.tools['suyu'] = found
            say('  suyu     %s' % found)
        elif strict:
            raise Fail('suyu not found (--suyu <folder containing src/core/hle/service/mii/types/raw_data.cpp>)')
    for tool in ('bash',):
        if not shutil.which(tool):
            raise Fail('%s not found on PATH (needed for runtime/build_glslang.sh)' % tool)

    # ---- test compile + link (+ run)
    probe = os.path.join(cx.build, 'probe')
    shutil.rmtree(probe, ignore_errors=True)
    os.makedirs(probe)
    inc = ['-I', os.path.join(sdl_lib, 'include'), '-I', os.path.join(cx.tools['vulkan'], 'include')]
    exe = os.path.join(probe, 'hello_sdl3_vulkan')
    cmd = [clang] + flags + ['-O1', '-w', '-fno-strict-aliasing', '-ffp-contract=off', os.path.join(REPO, 'toolchain', 'hello_sdl3_vulkan.c')] + inc + \
          ['-L' + os.path.join(sdl_lib, 'lib'), '-lSDL3', '-Wl,-rpath,$ORIGIN', '-Wl,-rpath-link,' + os.path.join(sdl_lib, 'lib'), '-o', exe]
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode:
        raise Fail('test compile/link of hello_sdl3_vulkan.c failed:\n$ %s\n%s' % (' '.join(cmd), r.stderr[-1500:]))
    with open(exe, 'rb') as f:
        hdr = f.read(20)
    if hdr[:4] != b'\x7fELF' or hdr[18:20] != b'\xb7\x00':
        raise Fail('test program is not an aarch64 ELF')
    say('  compile + link of an SDL3 + Vulkan program for aarch64: ok')
    shim_o = os.path.join(probe, 'shim.o')
    r = subprocess.run([clang] + flags + ['-c', '-O1', '-w', '-fPIC', '-fvisibility=hidden', '-I', os.path.join(cx.tools['glslang'], 'include'),
                                          os.path.join(REPO, 'runtime', 'vk_glslang_shim.c'), '-o', shim_o], capture_output=True, text=True, env=env)
    if r.returncode:
        raise Fail('runtime/vk_glslang_shim.c does not compile against the glslang headers in %s:\n%s' % (cx.tools['glslang'], r.stderr[-1500:]))
    say('  glslang shim compiles against %s: ok' % cx.tools['glslang'])
    # run it: native, or on the device over ssh
    libsdl = os.path.realpath(os.path.join(sdl_lib, 'lib', 'libSDL3.so.0' if os.path.exists(os.path.join(sdl_lib, 'lib', 'libSDL3.so.0')) else 'libSDL3.so'))
    shutil.copy2(libsdl, os.path.join(probe, 'libSDL3.so.0'))
    device = os.environ.get('SSBU_DEVICE')
    runenv = {k: v for k, v in os.environ.items()}
    if not runenv.get('WAYLAND_DISPLAY') and not runenv.get('DISPLAY'):
        runenv['SDL_VIDEO_DRIVER'] = runenv.get('SDL_VIDEO_DRIVER', 'offscreen')
    if cx.native:
        r = subprocess.run([exe, '--no-window'], capture_output=True, text=True, env=runenv, timeout=60)
        out = r.stdout + r.stderr
    elif device:
        remote = DEFAULT_DEVICE_DIR
        ssh = ['ssh', '-o', 'BatchMode=yes', device]
        subprocess.run(ssh + ['mkdir -p %s' % remote], check=True, capture_output=True)
        subprocess.run(['scp', '-q', '-o', 'BatchMode=yes', exe, os.path.join(probe, 'libSDL3.so.0'), '%s:%s/' % (device, remote)], check=True, capture_output=True)
        r = subprocess.run(ssh + ['cd %s && XDG_RUNTIME_DIR=/run/user/$(id -u) SDL_VIDEO_DRIVER=${SDL_VIDEO_DRIVER:-offscreen} timeout 60 ./hello_sdl3_vulkan --no-window' % remote],
                           capture_output=True, text=True)
        out = r.stdout + r.stderr
    else:
        say('  not run (cross build without SSBU_DEVICE=steamos@frame)')
        out = None
    if out is not None:
        gpu = next((l for l in out.splitlines() if l.startswith('GPU ')), None)
        if r.returncode == 0 and gpu:
            say('  run on %s: %s' % ('the device' if not cx.native else 'this machine', gpu.strip()))
        else:
            msg = 'the test program ran but did not reach a GPU (exit %s): %s' % (r.returncode, ' | '.join(out.strip().splitlines()[-3:]))
            if strict and cx.native:
                raise Fail(msg)
            say('  WARNING: ' + msg)
    cx.state['target'] = cx.target
    cx.state['target_flags'] = cx.target_flags
    cx.state['tools'] = cx.tools


def step_deps(cx):
    a = cx.a
    say('  python %s' % sys.version.split()[0])
    missing = []
    for mod, pkg in (('numpy', 'numpy'), ('lz4.block', 'lz4'), ('zstandard', 'zstandard'), ('PIL', 'Pillow'), ('unicorn', 'unicorn')):
        try:
            __import__(mod)
        except ImportError:
            missing.append(pkg)
    strict = not cx.linux or bool(a.dump or a.exefs or a.romfs)      # linux-arm64 without --dump: only the toolchain is probed
    if missing and strict:
        raise Fail('missing Python packages: %s   (pip install %s)' % (', '.join(missing), ' '.join(missing)))
    if missing:
        say('  WARNING: Python packages missing for the later steps: %s   (pip install %s)' % (', '.join(missing), ' '.join(missing)))
    else:
        say('  python packages: numpy lz4 zstandard Pillow unicorn: ok')

    if cx.linux:
        deps_linux(cx, strict)
    else:
        deps_windows(cx)

    # the dump
    dump = a.dump
    exefs = a.exefs or (os.path.join(dump, 'exefs') if dump else None)
    romfs = a.romfs or (os.path.join(dump, 'romfs') if dump else None)
    if not exefs or not romfs:
        if not strict:
            say('  no --dump given: toolchain checks only')
            cx.state['target'], cx.state['target_flags'] = cx.target, cx.target_flags
            cx.state['tools'] = cx.tools
            return
        raise Fail('--dump <folder> is required (or --exefs and --romfs)')
    for f in ('main', 'sdk'):
        if not os.path.isfile(os.path.join(exefs, f)):
            raise Fail('%s not found: the ExeFS folder needs the files main and sdk' % os.path.join(exefs, f))
    if not os.path.isfile(os.path.join(romfs, 'data.arc')):
        raise Fail('%s not found: the RomFS folder needs data.arc' % os.path.join(romfs, 'data.arc'))
    cx.state['dump'] = {'exefs': os.path.abspath(exefs), 'romfs': os.path.abspath(romfs)}
    sysdata = a.sysdata or (os.path.join(dump, 'sysdata') if dump else None)
    if sysdata and os.path.isdir(os.path.join(sysdata, 'fonts')):
        cx.state['dump']['sysdata'] = os.path.abspath(sysdata)
        say('  dump: exefs, romfs, sysdata (fonts%s)' % (', zoneinfo' if os.path.isdir(os.path.join(sysdata, 'zoneinfo')) else ''))
    else:
        say('  dump: exefs, romfs   (no sysdata%sfonts: the game runs but its text uses no system font)' % os.sep)
    if a.sdk_roots:
        if not os.path.isfile(a.sdk_roots):
            raise Fail('--sdk-roots file not found')
        cx.state['sdk_roots'] = os.path.abspath(a.sdk_roots)
    elif not cx.state.get('sdk_roots'):
        cx.state['sdk_roots'] = os.path.join(REPO, 'tools', 'lifter', 'sdklift_roots.txt')
    cx.state['tools'] = cx.tools
    cx.state['target'] = cx.target
    if cx.linux:
        cx.state['target_flags'] = cx.target_flags


def copy_if_changed(src, dst):
    if os.path.exists(dst) and os.path.getsize(dst) == os.path.getsize(src):
        with open(src, 'rb') as a, open(dst, 'rb') as b:
            if a.read() == b.read():
                return False
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    return True


def step_stage(cx):
    """The sources are laid out the way the tools expect them (one folder, ro_study\\, tools_audit\\, fighter_sdk\\, tests\\),
    next to binaries\\ and romfs: the tools address the dump by relative paths."""
    plan = [('runtime', '', None), ('tools/lifter', '', None), ('tools/nro', 'ro_study', '.py'), ('tools/audit', 'tools_audit', '.py'),
            ('fighter_sdk', 'fighter_sdk', None), ('tests', 'tests', None), ('mods', '', None)]
    n = changed = 0
    for src, dst, ext in plan:
        for root, dirs, files in os.walk(os.path.join(REPO, src)):
            dirs[:] = [d for d in dirs if d != '__pycache__']
            for f in files:
                if ext and not f.endswith(ext):
                    continue
                rel = os.path.relpath(os.path.join(root, f), os.path.join(REPO, src))
                n += 1
                changed += copy_if_changed(os.path.join(root, f), os.path.join(cx.work, dst, rel))
    for d in ('obj_host', os.path.join('tests', 'logs')):
        os.makedirs(os.path.join(cx.work, d), exist_ok=True)
    say('  %d source files, %d copied into %s' % (n, changed, cx.work))


def step_convert(cx):
    d = cx.state['dump']
    os.makedirs(cx.bin, exist_ok=True)
    conv = os.path.join(REPO, 'tools', 'extract', 'nso2elf.py')
    for name, want in (('main', GAME_TEXT_SIZE), ('sdk', SDK_TEXT_SIZE)):
        out = cx.run('convert', [sys.executable, conv, os.path.join(d['exefs'], name), os.path.join(cx.bin, name + '.elf')], cwd=REPO, log='convert_' + name)
        seg0 = next((l for l in out.splitlines() if l.startswith('seg 0')), '')
        got = int(seg0.split('size ')[1].split()[0], 16) if 'size ' in seg0 else 0
        if 'MISMATCH' in out or got != want:
            raise Fail('%s is not the expected build (code size %#x, expected %#x for 13.0.4, or a segment hash mismatch): '
                       'this port only handles Super Smash Bros. Ultimate 13.0.4 (update applied)' % (name, got, want))
        say('  %s -> %s.elf   code %.1f MB, segment hashes ok' % (name, name, got / 1048576.0))
    for name in os.listdir(d['exefs']):                 # main.npdm (main thread stack size) and the other ExeFS files sit beside the ELFs
        if os.path.isfile(os.path.join(d['exefs'], name)):
            shutil.copy2(os.path.join(d['exefs'], name), os.path.join(cx.bin, name))
    if not os.path.isfile(os.path.join(cx.bin, 'main.npdm')):
        raise Fail('exefs%smain.npdm is missing: dump the whole ExeFS (main, sdk, rtld, subsdk0, main.npdm)' % os.sep)
    if d.get('sysdata'):
        for sub in ('fonts', 'zoneinfo'):
            if os.path.isdir(os.path.join(d['sysdata'], sub)):
                dst = os.path.join(cx.build, 'sysdata', sub)
                if not os.path.isdir(dst):
                    shutil.copytree(os.path.join(d['sysdata'], sub), dst)


def step_modules(cx):
    ro = os.path.join(cx.work, 'ro_study')
    env = cx.env(SSBU_ARC=os.path.join(cx.state['dump']['romfs'], 'data.arc'))
    if not os.path.isdir(os.path.join(ro, 'nro')) or not any(f.endswith('.nro') for f in os.listdir(os.path.join(ro, 'nro'))):
        cx.run('modules', [sys.executable, 'arc_extract.py', 'nro', 'nro'], cwd=ro, env=env)
    n = len([f for f in os.listdir(os.path.join(ro, 'nro')) if f.endswith('.nro')])
    cx.run('modules', [sys.executable, 'lift_all.py', 'map'], cwd=ro, env=env)
    cx.run('modules', [sys.executable, 'build_nro.py'], cwd=cx.work)          # writes nro_registry.c from ro_study\nro_map.json
    say('  %d script modules extracted from data.arc, nro_map.json and nro_registry.c written' % n)


STUB_LIST = '/* no audit data: the strict-mode lists are empty (the port runs the same; SSBU_STRICT then reports nothing) */\nstatic const char *const %s[] = {\n    "",\n};\n'


def step_tables(cx):
    w = cx.work
    py = sys.executable
    cx.run('tables', [py, 'gen_imports.py'])
    cx.run('tables', [py, 'gen_sdkdata.py'])
    cx.run('tables', [py, 'gen_sdktest.py'])
    cx.run('tables', [py, 'gen_quad_tests.py'])
    cx.run('tables', [py, 'gen_eh_tests.py'])
    for name, var in (('fake_whitelist.h', 'audit_whitelist'), ('fake_openlist.h', 'audit_openlist')):
        if not os.path.exists(os.path.join(w, name)):
            with open(os.path.join(w, name), 'w', newline='\n') as f:
                f.write(STUB_LIST % var)
    say('  imports_all.txt, sdkdata.c, sdktest_data.h, quad_tests.h, eh_tests.h, strict-mode lists (empty)')


def step_lift(cx):
    out = cx.run('lift', [sys.executable, 'recomp_eh.py', 'gen_eh_o2', str(cx.jobs)])
    n = len([f for f in os.listdir(os.path.join(cx.work, 'gen_eh_o2')) if f.endswith('.c')])
    say('  main lifted into gen_eh_o2: %d C files (%.0f MB)' % (n, disk_mb(os.path.join(cx.work, 'gen_eh_o2'))))


def install_header(roots_text):
    """sdklift_install.h = the names of the managed block of sdklift_roots.txt (what tools/audit/lift_select.py writes)."""
    import re
    blk = re.search(r'# >>> audit block.*?# <<< audit block', roots_text, re.S)
    names = []
    if blk:
        names = [l.strip() for l in blk.group(0).splitlines() if l.strip() and not l.startswith('#') and l.split()[0] not in ('cut', 'lift', 'trap', 'addr')]
    return ('/* generated by scripts/setup.py from the managed block of sdklift_roots.txt: imports whose handler is the real code lifted from sdk.elf,\n'
            ' * installed over the host re-implementation by nnsdk2.c */\nstatic const char *const sdk_install_names[] = {\n' +
            ''.join('    "%s",\n' % n for n in names) + '};\n')


def step_sdk(cx):
    roots = cx.state.get('sdk_roots')
    if not roots or not os.path.isfile(roots):
        raise Fail('the list of SDK functions to lift was not found (tools/lifter/sdklift_roots.txt, or --sdk-roots <file>)')
    shutil.copyfile(roots, os.path.join(cx.work, 'sdklift_roots.txt'))
    with open(roots, encoding='utf-8', errors='replace') as f:
        text = f.read()
    with open(os.path.join(cx.work, 'sdklift_install.h'), 'w', newline='\n') as f:
        f.write(install_header(text))
    out = cx.run('sdk', [sys.executable, 'sdklift.py', 'gen'])
    line = next((l for l in out.splitlines() if l.startswith('functions lifted')), '')
    say('  ' + (line or 'sdklift_gen.c written'))


def build_env(cx):
    return cx.env(RECOMP_GEN='gen_eh_o2', RECOMP_OBJ='obj_eh_o1', RECOMP_DLL=cx.game_lib, RECOMP_NRO_SUFFIX='_eh_o2')


def step_compile(cx):
    cx.run('compile', [sys.executable, 'build_opt.py', 'main', str(cx.jobs)], env=build_env(cx))
    dll = os.path.join(cx.work, cx.game_lib)
    say('  %s %.0f MB; objects %.0f MB' % (cx.game_lib, os.path.getsize(dll) / 1048576.0, disk_mb(os.path.join(cx.work, 'obj_eh_o1'))))


def step_nro(cx):
    mods = [m for m in (cx.a.modules or 'all').split(',') if m]
    argv = [sys.executable, 'build_opt.py', 'nro', str(cx.jobs)] + ([] if mods == ['all'] else mods)
    cx.run('nro', argv, env=build_env(cx))
    d = os.path.join(cx.work, 'nro_bin_eh_o2')
    say('  %d module libraries in nro_bin_eh_o2 (%.0f MB)' % (len([f for f in os.listdir(d) if f.endswith(cx.so)]), disk_mb(d)))


def step_link(cx):
    if cx.linux:
        env = cx.env(CC=cx.tools['clang'], CFLAGS_TARGET=' '.join(shlex.quote(f) for f in cx.target_flags),
                     GLSLANG_ROOT=cx.tools['glslang'], LIBDIR=os.path.join(cx.tools['glslang'], 'lib'))
        cx.run('link', ['bash', os.path.join(cx.work, 'build_glslang.sh')], env=env)
        bundle = ['libssbu_glslang.so', 'libSDL3.so.0', cx.game_lib]
    else:
        cx.run('link', [os.environ.get('COMSPEC', 'cmd.exe'), '/c', os.path.join(cx.work, 'build_glslang.bat')], env=cx.env())
        bundle = ['SDL3.dll', 'ssbu_glslang.dll', 'ssbu_game_eh_o1.dll']
    cx.run('link', [sys.executable, 'build.py', 'link'], env=cx.env(**{k: v for k, v in build_env(cx).items() if k != 'RECOMP_NRO_SUFFIX'}, RECOMP_EXE=cx.exe))
    exe = os.path.join(cx.work, cx.exe)
    for f in [cx.exe] + bundle:
        if not os.path.exists(os.path.join(cx.work, f)):
            raise Fail('%s was not produced' % f)
    say('  %s %.1f MB, %s beside it' % (cx.exe, os.path.getsize(exe) / 1048576.0, ', '.join(bundle)))


def game_env(cx, **extra):
    d = cx.state['dump']
    e = cx.env(SSBU_ROMFS=d['romfs'], **extra)
    if cx.linux:
        e.setdefault('XDG_RUNTIME_DIR', '/run/user/%d' % os.getuid())
        if not e.get('WAYLAND_DISPLAY') and not e.get('DISPLAY'):
            e.setdefault('SDL_VIDEO_DRIVER', 'offscreen')      # started from ssh / a console: no desktop session
    if os.path.isdir(os.path.join(cx.build, 'sysdata', 'fonts')):
        e['SSBU_FONTS'] = os.path.join(cx.build, 'sysdata', 'fonts')
    if os.path.isdir(os.path.join(cx.build, 'sysdata', 'zoneinfo')):
        e['SSBU_TZDIR'] = os.path.join(cx.build, 'sysdata', 'zoneinfo')
    return e


def step_check(cx):
    if cx.linux and not cx.native:      # the aarch64 exe cannot run on the build host: do it on the device
        say('  cross build: the checks run the aarch64 exe, so run them on the Frame:  python3 scripts/setup.py --target linux-arm64 --only check')
        say('  (rsync/scp the repo and build/linux-arm64/ to the device first; see BUILDING.md "Linux / Steam Frame"). Skipped here.')
        return
    exe = os.path.join(cx.work, cx.exe)
    main_elf = os.path.join(cx.bin, 'main.elf')
    scratch = os.path.join(cx.build, 'selftest_userdata')
    shutil.rmtree(scratch, ignore_errors=True)
    os.makedirs(scratch)
    out = cx.run('check', [exe, main_elf, '60'], env=game_env(cx, SSBU_SDK_SELFTEST='1', SSBU_USERDATA=scratch), log='check_sdk', ok=(0,))
    line = next((l for l in out.splitlines() if 'SDK self-test' in l), '')
    if '0 failure(s)' not in line:
        raise Fail('SSBU_SDK_SELFTEST: ' + (line or 'no result line'))
    say('  SSBU_SDK_SELFTEST: ' + line.strip())
    out = cx.run('check', [exe, main_elf, '5'], env=game_env(cx, SSBU_PAD_SELFTEST='1', SSBU_USERDATA=scratch), log='check_pad')
    line = next((l for l in reversed(out.splitlines()) if 'pad' in l.lower() and '/' in l), out.strip().splitlines()[-1] if out.strip() else '')
    say('  SSBU_PAD_SELFTEST: ' + line.strip())
    # the lifter's differential test against the Unicorn emulator, on a sample of the instruction shapes of main
    py, w = sys.executable, cx.work
    cx.run('check', [py, 'difftest.py', 'sample'], log='check_difftest')
    cx.run('check', [py, 'difftest.py', 'gen'], log='check_difftest')
    dt = 'difftest' if cx.linux else 'difftest.exe'
    cx.run('check', [cx.tools['clang']] + cx.target_flags + ['-O0', '-w', '-fno-strict-aliasing'] + (['-ffp-contract=off'] if cx.linux else []) +
           ['-I', '.', 'difftest_gen.c', 'difftest_host.c', '-o', dt] + (['-ldl', '-lm'] if cx.linux else []), log='check_difftest')
    out = cx.run('check', [py, 'difftest.py', 'check'], env=cx.env(DIFFTEST_EXE=os.path.join(w, dt)), log='check_difftest')
    line = next((l for l in out.splitlines() if l.startswith('tests:')), '')
    if not line:
        raise Fail('difftest printed no result')
    say('  differential test: ' + line.strip())
    if 'different: 0' not in line:
        say('  WARNING: the differential test reports differences (see logs/check_difftest.log)')


RUNNERS = {'deps': step_deps, 'stage': step_stage, 'convert': step_convert, 'modules': step_modules, 'tables': step_tables,
           'lift': step_lift, 'sdk': step_sdk, 'compile': step_compile, 'nro': step_nro, 'link': step_link, 'check': step_check}
ALWAYS = {'deps', 'stage'}               # cheap, and they fill the in-memory context


def how_to_run(cx):
    d = cx.state.get('dump', {})
    say('')
    say('Start the game (from a shell; the window opens at once, the first run compiles shaders and is slow):')
    say('  python scripts/run.py %s--userdata <a folder for your saves> [--seconds 0]' % ('--build %s ' % os.path.relpath(cx.build, REPO) if cx.linux else ''))
    if cx.linux:
        say('or by hand:   cd %s && ./%s ../binaries/13.0.4/main.elf 0' % (os.path.relpath(cx.work), cx.exe))
    else:
        say('or by hand:   cd build\\work   and run   %s ..\\binaries\\13.0.4\\main.elf 0' % EXE)
    say('with the environment variables SSBU_ROMFS=%s  SSBU_USERDATA=<saves>  (options: BUILDING.md)' % d.get('romfs', '<romfs>'))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dump', help='folder with exefs\\ and romfs\\ (and optionally sysdata\\) dumped from your console')
    ap.add_argument('--exefs'), ap.add_argument('--romfs'), ap.add_argument('--sysdata')
    ap.add_argument('--target', choices=('windows', 'linux-arm64'), default='windows', help='windows (default) or linux-arm64 (Steam Frame; cross from WSL/Linux or native on the device)')
    ap.add_argument('--sysroot', help='linux-arm64 cross: sysroot copied from the device (toolchain/sync_sysroot.sh; default $SYSROOT or ~/sysroots/steam-frame)')
    ap.add_argument('--build', help='build folder (default <repo>/build; linux-arm64: <repo>/build/linux-arm64)')
    ap.add_argument('--jobs', type=int, help='parallel compiler jobs (default min(24, cpus))')
    ap.add_argument('--llvm', help='LLVM 9.0.0 folder (bin\\clang.exe); linux-arm64: any LLVM >= 9 folder with bin/clang and bin/ld.lld')
    ap.add_argument('--sdl3', help='SDL3 development package (VC) folder: include\\, lib\\x64\\; linux-arm64: an SDL3 install prefix (include/, lib/libSDL3.so)')
    ap.add_argument('--vulkan-headers', help='Khronos Vulkan-Headers folder (include\\vulkan\\)')
    ap.add_argument('--glslang', help='glslang windows-x86_64 release folder (include\\, lib\\)')
    ap.add_argument('--suyu', help='source checkout of a yuzu-family emulator (only src\\core\\hle\\service\\mii is read)')
    ap.add_argument('--sdk-roots', help='another list of SDK functions to lift (default: tools/lifter/sdklift_roots.txt)')
    ap.add_argument('--modules', help='script modules to build: all (default) or a comma list, e.g. lua2cpp_common,lua2cpp_mario')
    ap.add_argument('--redo', help='repeat this step')
    ap.add_argument('--from', dest='from_step', help='repeat this step and all after it')
    ap.add_argument('--only', help='run only this step')
    ap.add_argument('--status', action='store_true'), ap.add_argument('--steps', action='store_true'), ap.add_argument('--run', action='store_true')
    a = ap.parse_args()
    if a.steps:
        say(' '.join(STEPS))
        return 0
    cx = Ctx(a)
    if a.status:
        for s in STEPS:
            r = cx.state.get('done', {}).get(s)
            say('%-8s %s' % (s, ('done  %s' % fmt(r['secs'])) if r else '-'))
        return 0
    if a.run:
        how_to_run(cx)
        return 0
    todo = STEPS
    done = cx.state.setdefault('done', {})
    force = set()
    for opt in (a.redo, a.from_step, a.only):
        if opt and opt not in STEPS:
            say('unknown step %s (steps: %s)' % (opt, ' '.join(STEPS)))
            return 2
    if a.redo:
        force.add(a.redo)
    if a.from_step:
        force |= set(STEPS[STEPS.index(a.from_step):])
    if a.only:
        todo = sorted({'deps', 'stage', a.only}, key=STEPS.index)
        force.add(a.only)
    t_all = time.time()
    for i, s in enumerate(STEPS):
        if s not in todo:
            continue
        if s in done and s not in force and s not in ALWAYS:
            say('[%d/%d] %s: done earlier (%s), skipped' % (i + 1, len(STEPS), s, fmt(done[s]['secs'])))
            continue
        say('[%d/%d] %s ...' % (i + 1, len(STEPS), s))
        t0 = time.time()
        try:
            RUNNERS[s](cx)
        except Fail as e:
            say('FAILED at step %s: %s' % (s, e))
            say('fix the problem and run the same command again: finished steps are skipped.')
            cx.save()
            return 1
        secs = time.time() - t0
        if s not in ALWAYS or s not in done:
            done[s] = {'secs': secs, 'at': time.strftime('%Y-%m-%d %H:%M')}
        cx.save()
        say('[%d/%d] %s: ok  %s' % (i + 1, len(STEPS), s, fmt(secs)))
    say('')
    say('all steps finished in %s; build folder %.0f MB' % (fmt(time.time() - t_all), disk_mb(cx.build)))
    how_to_run(cx)
    return 0


if __name__ == '__main__':
    sys.exit(main())
