#!/usr/bin/env python3
"""ror-build: builds the game executable on the player's machine from the
player's own disc image.

    ror-build --iso PATH --out DIR [--jobs N]
    ror-build --check --iso PATH --out DIR

A release holds everything except the code translated from the game: the
launcher, the runtime as prebuilt objects, the recompiler, a C compiler (Zig)
and a Python to run this file. This tool takes the game executable out of the
disc image, recompiles it to C, translates the VU1 micro program, compiles all
of that with the bundled compiler and links it with the runtime into
DIR/ror-game (ror-game.exe on Windows).

Output (stdout, UTF-8, one line per step, flushed):
    PROGRESS <done> <total> <text>
    DONE <path of the executable>        exit 0
    ERROR <one line>                     exit 1
--check prints "up to date" (exit 0) or "needs build: <reason>" (exit 1).
The full log is DIR/build.log. Intermediate files live in DIR/.build and are
removed on success; the executable is put in place by one rename at the very
end, so an interrupted build leaves no partial ror-game. Nothing is written
outside DIR and nothing is fetched from anywhere.

Only the Python standard library is used. The file runs from the kit folder
(buildkit/ beside the launcher), which may be read-only.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import signal
import struct
import subprocess
import sys
import threading

KIT = os.path.dirname(os.path.abspath(__file__))
WINDOWS = os.name == 'nt'
EXE = '.exe' if WINDOWS else ''
GAME_ELF = 'SLES_542.18'
# sha256 of the executable on the European disc (SLES-54218); other editions are not supported
ELF_SHA256 = '71417965b8931cadcebc33f48439df3290822316e4c139d9c4c7a4b6d7fb7692'

COMMON = ['-fno-sanitize=undefined', '-g0', '-O2', '-fwrapv', '-fno-strict-aliasing', '-fno-math-errno',
          '-ffp-contract=off', '-w', '-DGS_HW']
if WINDOWS:
    TARGET = ['-target', 'x86_64-windows-gnu']
    CFLAGS = TARGET + COMMON + ['-D_WIN32_WINNT=0x0A00', '-DUNICODE', '-D_UNICODE', '-include', 'rt/win_compat.h']
    LINK_LIBS = ['-lwinmm', '-lgdi32', '-luser32', '-lshell32', '-Wl,--stack,8388608']
else:
    TARGET = ['-target', 'x86_64-linux-gnu.2.31']
    CFLAGS = TARGET + COMMON
    LINK_LIBS = ['-lm', '-lpthread', '-ldl']


class BuildError(Exception):
    pass


def remove_tree(path):
    """Delete a folder of ours, read-only entries included (files copied from a read-only install keep that bit)."""
    def retry(func, p, _exc):
        try:
            os.chmod(os.path.dirname(p), 0o700)
            os.chmod(p, 0o700)
            func(p)
        except OSError:
            pass
    if os.path.isdir(path):
        for d, dirs, _files in os.walk(path):
            for n in dirs:
                try:
                    os.chmod(os.path.join(d, n), 0o700)
                except OSError:
                    pass
        shutil.rmtree(path, onerror=retry)


def say(line):
    sys.stdout.write(line + '\n')
    sys.stdout.flush()


def read_elf_from_iso(iso):
    """The game executable from the root directory of the ISO 9660 image; BuildError if this is not the disc."""
    try:
        with open(iso, 'rb') as f:
            f.seek(16 * 2048)
            pvd = f.read(2048)
            if len(pvd) < 2048 or pvd[1:6] != b'CD001':
                raise BuildError('not a disc image (no ISO 9660 volume): ' + iso)
            lba, size = struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0]
            if size > 1 << 20:
                raise BuildError('not a Rule of Rose disc image: ' + iso)
            f.seek(lba * 2048)
            d = f.read(size)
            o = 0
            while o < len(d):
                n = d[o]
                if n == 0:
                    o = (o // 2048 + 1) * 2048
                    continue
                name = d[o + 33:o + 33 + d[o + 32]].decode('latin1').split(';')[0].upper()
                if name == GAME_ELF and not d[o + 25] & 2:
                    flba, fsize = struct.unpack_from('<I', d, o + 2)[0], struct.unpack_from('<I', d, o + 10)[0]
                    if fsize > 64 << 20:
                        break
                    f.seek(flba * 2048)
                    data = f.read(fsize)
                    if len(data) != fsize:
                        raise BuildError('the disc image is truncated: ' + iso)
                    return data
                o += n
    except OSError as e:
        raise BuildError('cannot read the disc image: %s' % (e.strerror or e))
    raise BuildError('this is not the European Rule of Rose disc (SLES-54218): no %s in the image' % GAME_ELF)


def kit_text(name):
    with open(os.path.join(KIT, name), 'r', encoding='utf-8') as f:
        return f.read().strip()


def identity(iso, elf):
    """What the built executable depends on; stored in ror-game.stamp."""
    return {'iso_size': os.path.getsize(iso), 'elf_sha256': hashlib.sha256(elf).hexdigest(),
            'version': kit_text('VERSION'), 'kit': kit_text('KITHASH')}


def check(iso, out):
    try:
        elf = read_elf_from_iso(iso)
    except BuildError as e:
        say('needs build: ' + str(e))
        return 1
    want = identity(iso, elf)
    game, stamp = os.path.join(out, 'ror-game' + EXE), os.path.join(out, 'ror-game.stamp')
    if not os.path.isfile(game):
        say('needs build: no ror-game yet')
        return 1
    try:
        with open(stamp, 'r', encoding='utf-8') as f:
            have = json.load(f)
    except (OSError, ValueError):
        say('needs build: no stamp')
        return 1
    for k in ('elf_sha256', 'iso_size', 'version', 'kit'):
        if have.get(k) != want[k]:
            say('needs build: %s changed' % {'elf_sha256': 'disc image', 'iso_size': 'disc image', 'version': 'release',
                                              'kit': 'runtime'}[k])
            return 1
    say('up to date')
    return 0


# ---- child processes: they must not outlive this one (a cancelled build is a killed ror-build) ----
_children = set()
_children_lock = threading.Lock()
_cancelled = False

if WINDOWS:
    def _tie_children_to_us():
        """A job object that kills everything in it when its last handle closes, i.e. when this process ends."""
        import ctypes
        from ctypes import wintypes
        k32 = ctypes.WinDLL('kernel32', use_last_error=True)
        k32.CreateJobObjectW.restype = wintypes.HANDLE
        k32.GetCurrentProcess.restype = wintypes.HANDLE
        k32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
        k32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]

        class BASIC(ctypes.Structure):
            _fields_ = [('PerProcessUserTimeLimit', ctypes.c_int64), ('PerJobUserTimeLimit', ctypes.c_int64),
                        ('LimitFlags', wintypes.DWORD), ('MinimumWorkingSetSize', ctypes.c_size_t),
                        ('MaximumWorkingSetSize', ctypes.c_size_t), ('ActiveProcessLimit', wintypes.DWORD),
                        ('Affinity', ctypes.c_size_t), ('PriorityClass', wintypes.DWORD), ('SchedulingClass', wintypes.DWORD)]

        class IO(ctypes.Structure):
            _fields_ = [(n, ctypes.c_uint64) for n in ('r', 'w', 'o', 'rt', 'wt', 'ot')]

        class EXT(ctypes.Structure):
            _fields_ = [('Basic', BASIC), ('Io', IO), ('ProcessMemoryLimit', ctypes.c_size_t),
                        ('JobMemoryLimit', ctypes.c_size_t), ('PeakProcessMemoryUsed', ctypes.c_size_t),
                        ('PeakJobMemoryUsed', ctypes.c_size_t)]
        job = k32.CreateJobObjectW(None, None)
        if not job:
            return
        info = EXT()
        info.Basic.LimitFlags = 0x2000                      # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        k32.SetInformationJobObject(job, 9, ctypes.byref(info), ctypes.sizeof(info))
        k32.AssignProcessToJobObject(job, k32.GetCurrentProcess())      # children inherit the job

    _preexec = None
else:
    def _tie_children_to_us():
        pass

    def _preexec():
        import ctypes
        os.setsid()                                         # own process group: the compiler starts helpers of its own
        ctypes.CDLL(None).prctl(1, signal.SIGKILL)          # PR_SET_PDEATHSIG: die with the parent


def run(cmd, cwd, env, log, stdout_path=None):
    """Run one tool; output goes to the log (or stdout to a file). BuildError on failure."""
    if _cancelled:
        raise BuildError('cancelled')
    out = open(stdout_path, 'wb') if stdout_path else subprocess.PIPE
    try:
        p = subprocess.Popen(cmd, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=out,
                             stderr=subprocess.PIPE if stdout_path else subprocess.STDOUT,
                             preexec_fn=_preexec, creationflags=0x08000000 if WINDOWS else 0)   # CREATE_NO_WINDOW
    except OSError as e:
        raise BuildError('cannot start %s: %s' % (os.path.basename(cmd[0]), e.strerror or e))
    finally:
        if stdout_path:
            out.close()
    with _children_lock:
        _children.add(p)
    text = p.communicate()[1 if stdout_path else 0] or b''
    with _children_lock:
        _children.discard(p)
    with log_lock:
        log.write('$ ' + ' '.join(cmd) + '\n' + text.decode('utf-8', 'replace'))
        log.flush()
    if p.returncode != 0:
        tail = text.decode('utf-8', 'replace').strip().splitlines()
        raise BuildError('%s failed (exit %d)%s' % (os.path.basename(cmd[0]), p.returncode, ': ' + tail[-1][:200] if tail else ''))


log_lock = threading.Lock()


def _on_signal(signum, frame):
    global _cancelled
    _cancelled = True
    with _children_lock:
        for p in list(_children):
            try:
                if WINDOWS:
                    p.kill()                                # (the job object takes the rest down)
                else:
                    os.killpg(p.pid, signal.SIGKILL)
            except OSError:
                pass
    raise KeyboardInterrupt


def build(iso, out, jobs):
    os.makedirs(out, exist_ok=True)
    work = os.path.join(out, '.build')
    try:
        return build_in(iso, out, jobs, work)
    except BaseException:
        remove_tree(work)        # nothing made from the game is left behind
        raise


def build_in(iso, out, jobs, work):
    game = os.path.join(out, 'ror-game' + EXE)
    remove_tree(work)
    os.makedirs(os.path.join(work, 'gen'))
    log = open(os.path.join(out, 'build.log'), 'w', encoding='utf-8')
    log.write('ror-build %s, kit %s, %s, %d jobs\n' % (kit_text('VERSION'), kit_text('KITHASH')[:16], sys.platform, jobs))

    env = {k: v for k, v in os.environ.items() if not k.startswith('PYTHON')}     # the bundled Python, nothing of the user's
    env['PYTHONNOUSERSITE'] = '1'
    env['ZIG_GLOBAL_CACHE_DIR'] = os.path.join(work, 'zig-cache')
    env['ZIG_LOCAL_CACHE_DIR'] = os.path.join(work, 'zig-cache')
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    zig = os.path.join(KIT, 'zig', 'zig' + EXE)
    python = sys.executable

    total = [5]                                        # disc, recompile, VU1, (files), link, install: fixed once the files are known

    def progress(done, text):
        say('PROGRESS %d %d %s' % (done, total[0], text))

    progress(0, 'Reading the disc image')
    elf = read_elf_from_iso(iso)
    ident = identity(iso, elf)
    if ident['elf_sha256'] != ELF_SHA256:
        raise BuildError('this is not the European Rule of Rose disc (SLES-54218): its %s is a different version' % GAME_ELF)
    with open(os.path.join(work, GAME_ELF), 'wb') as f:
        f.write(elf)

    progress(1, 'Recompiling the game code')
    run([python, '-B', os.path.join(KIT, 'recomp', 'gen.py'), GAME_ELF, 'gen'], work, env, log)

    progress(2, 'Translating the VU1 program')
    run([python, '-B', os.path.join(KIT, 'recomp', 'vu1extract.py'), GAME_ELF, 'vu1_micro.bin'], work, env, log)
    run([os.path.join(KIT, 'vugen' + EXE), 'gen', 'vu1_micro.bin'], work, env, log,
        stdout_path=os.path.join(work, 'gen', 'vu1_fast_gen.inc'))

    # The runtime sources that include generated headers are compiled here too; copied so that every path the
    # compiler sees is relative to the work folder.
    shutil.copytree(os.path.join(KIT, 'rt', 'src'), os.path.join(work, 'rt'))
    os.makedirs(os.path.join(work, 'obj'))
    sources = sorted('gen/' + n for n in os.listdir(os.path.join(work, 'gen')) if n.endswith('.c'))
    sources += sorted('rt/' + n for n in os.listdir(os.path.join(work, 'rt')) if n.endswith('.c'))
    total[0] = 5 + len(sources)
    done = [3]

    def compile_one(src):
        obj = 'obj/' + os.path.basename(src)[:-2] + '.o'
        run([zig, 'cc'] + CFLAGS + ['-Irt', '-Igen', '-c', src, '-o', obj], work, env, log)
        with log_lock:
            done[0] += 1
            progress(done[0], 'Compiling ' + os.path.basename(src))
        return obj

    progress(3, 'Compiling (%d files, %d at a time)' % (len(sources), jobs))
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(compile_one, s) for s in sources]
        try:
            objs = [f.result() for f in futures]
        except BaseException:
            for f in futures:
                f.cancel()
            raise

    progress(done[0], 'Linking')
    libdir = os.path.join(KIT, 'rt', 'lib')
    prebuilt = sorted(os.path.join(libdir, n) for n in os.listdir(libdir) if n.endswith(('.o', '.a')))
    with open(os.path.join(work, 'link.rsp'), 'w', encoding='utf-8') as f:      # a response file: the command line stays short
        for p in objs + prebuilt:
            f.write('"%s"\n' % p.replace('\\', '/'))
    run([zig, 'cc'] + TARGET + ['-o', 'ror-game' + EXE, '@link.rsp'] + LINK_LIBS + ['-s'], work, env, log)

    progress(done[0] + 1, 'Installing')
    with open(os.path.join(work, 'ror-game.stamp'), 'w', encoding='utf-8') as f:
        json.dump(ident, f)
    stamp = os.path.join(out, 'ror-game.stamp')
    if os.path.exists(stamp):
        os.remove(stamp)                               # no stamp while the executable is being replaced
    os.replace(os.path.join(work, 'ror-game' + EXE), game)
    os.replace(os.path.join(work, 'ror-game.stamp'), stamp)
    log.write('done: %s\n' % game)
    log.close()
    remove_tree(work)
    return game


def main():
    for stream in (sys.stdout, sys.stderr):                # the same bytes on every system: UTF-8
        try:
            stream.reconfigure(encoding='utf-8', errors='replace')
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(prog='ror-build', description='Build the Rule of Rose executable from your own disc image.')
    ap.add_argument('--iso', required=True, help='disc image of the European release (SLES-54218)')
    ap.add_argument('--out', required=True, help='folder to put ror-game in (created if needed)')
    ap.add_argument('--jobs', type=int, default=0, help='compilers to run at a time (default: one per processor)')
    ap.add_argument('--check', action='store_true', help='only report whether DIR/ror-game is up to date for this image')
    a = ap.parse_args()
    iso, out = os.path.abspath(a.iso), os.path.abspath(a.out)
    try:
        if a.check:
            return check(iso, out)
        _tie_children_to_us()
        signal.signal(signal.SIGTERM, _on_signal)
        signal.signal(signal.SIGINT, _on_signal)
        jobs = a.jobs if a.jobs > 0 else (os.cpu_count() or 2)
        game = build(iso, out, jobs)
        say('DONE ' + game)
        return 0
    except BuildError as e:
        say('ERROR ' + str(e))
    except KeyboardInterrupt:
        say('ERROR cancelled')
    except OSError as e:
        say('ERROR %s: %s' % (e.strerror or 'file error', e.filename or ''))
    return 1


if __name__ == '__main__':
    sys.exit(main())
