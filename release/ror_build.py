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
    PROGRESS <done> <total> <text>       total is always 100; done follows the time the build is expected to take;
                                         the text is meant for the player (plain words, no file names)
    DONE <path of the executable>        exit 0
    ERROR <one line>                     exit 1
--check prints "up to date" (exit 0) or "needs build: <reason>" (exit 1).
The full log is DIR/build.log. Intermediate files live in DIR/.build and are
removed on success; the executable is put in place by one rename at the very
end, so an interrupted build leaves no partial ror-game; a build that is
killed takes its compilers with it and its work folder is removed (at once on
Linux, by the next build or --check on Windows). Nothing is written outside
DIR and nothing is fetched from anywhere.

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
import time

KIT = os.path.dirname(os.path.abspath(__file__))
T_START = time.monotonic()
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


def pid_alive(pid):
    if pid <= 0:
        return False
    if WINDOWS:
        import ctypes
        k32 = ctypes.WinDLL('kernel32')
        k32.OpenProcess.restype = ctypes.c_void_p
        h = k32.OpenProcess(0x00100000, False, pid)        # SYNCHRONIZE
        if not h:
            return False
        alive = k32.WaitForSingleObject(ctypes.c_void_p(h), 0) == 0x102     # WAIT_TIMEOUT: still running
        k32.CloseHandle(ctypes.c_void_p(h))
        return alive
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except OSError:
        return True


def remove_stale_work(out):
    """The work folder of a build that was killed: remove it unless that build is still running."""
    work = os.path.join(out, '.build')
    if not os.path.isdir(work):
        return
    try:
        with open(os.path.join(work, 'pid'), 'r') as f:
            pid = int(f.read().strip() or 0)
    except (OSError, ValueError):
        pid = 0
    try:
        fresh = time.time() - os.path.getmtime(os.path.join(work, 'pid')) < 30     # a running build touches it every half second
    except OSError:
        fresh = False
    if pid != os.getpid() and fresh and pid_alive(pid):
        raise BuildError('another build is running in this folder')
    remove_tree(work)


def check(iso, out):
    try:
        remove_stale_work(out)
    except BuildError:
        say('needs build: a build is running')
        return 1
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
    def _tie_children_to_us(work=None):
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

    def _release_guard():
        pass

    _preexec = None
else:
    # Linux. This process makes itself a process group; the compilers and whatever they start stay in it.
    # A guard process outside the group holds the read end of a pipe from here: when this process ends
    # without saying goodbye (killed, crashed) the guard kills the group and removes the work folder.
    GUARD = r"""
import os, shutil, signal, sys, time
pgid, work = int(sys.argv[1]), sys.argv[2]
if sys.stdin.buffer.read() != b'done':
    try: os.killpg(pgid, signal.SIGKILL)
    except OSError: pass
    time.sleep(0.3)
    for d, dirs, _ in os.walk(work):
        for n in dirs:
            try: os.chmod(os.path.join(d, n), 0o700)
            except OSError: pass
    shutil.rmtree(work, ignore_errors=True)
"""
    _guard = None

    def _tie_children_to_us(work=None):
        global _guard
        try:
            os.setpgid(0, 0)
        except OSError:
            pass                                            # a session leader is a group leader already
        if work:
            _guard = subprocess.Popen([sys.executable, '-I', '-c', GUARD, str(os.getpgrp()), work], stdin=subprocess.PIPE,
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)

    def _release_guard():
        if _guard:
            try:
                _guard.stdin.write(b'done')
                _guard.stdin.close()
                _guard.wait(timeout=5)
            except (OSError, subprocess.TimeoutExpired):
                pass

    def _preexec():
        import ctypes
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
    t0 = time.monotonic()
    text = p.communicate()[1 if stdout_path else 0] or b''
    with _children_lock:
        _children.discard(p)
    with log_lock:
        log.write('$ ' + ' '.join(cmd) + '\n' + text.decode('utf-8', 'replace') + '  [%.1f s, at %.1f s]\n' % (time.monotonic() - t0, time.monotonic() - T_START))
        log.flush()
    if p.returncode != 0:
        tail = text.decode('utf-8', 'replace').strip().splitlines()
        raise BuildError('%s failed (exit %d)%s' % (os.path.basename(cmd[0]), p.returncode, ': ' + tail[-1][:200] if tail else ''))


log_lock = threading.Lock()


def _on_signal(signum, frame):
    global _cancelled
    _cancelled = True
    if WINDOWS:
        with _children_lock:
            for p in list(_children):
                try:
                    p.kill()                                # (the job object takes the rest down)
                except OSError:
                    pass
    else:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)       # the whole group, compilers' helpers included; not us
        try:
            os.killpg(os.getpgrp(), signal.SIGTERM)
        except OSError:
            pass
    raise KeyboardInterrupt


def build(iso, out, jobs):
    os.makedirs(out, exist_ok=True)
    work = os.path.join(out, '.build')
    remove_stale_work(out)
    os.makedirs(os.path.join(work, 'gen'))
    with open(os.path.join(work, 'pid'), 'w') as f:
        f.write(str(os.getpid()))
    _tie_children_to_us(work)
    signal.signal(signal.SIGTERM, _on_signal)
    signal.signal(signal.SIGINT, _on_signal)
    try:
        return build_in(iso, out, jobs, work)
    except BaseException:
        time.sleep(0.2)                                    # compilers that were just told to stop let go of their files
        remove_tree(work)                                  # nothing made from the game is left behind
        raise
    finally:
        _release_guard()


# Progress is reported out of 100 and follows time, not the number of steps: 0-1 the disc, 1-6 reading the
# game (recompiler, VU1 translation), 6-94 compiling, 94-99 linking and installing. Compile times as measured
# on the development machine (seconds, with all its 24 threads busy); only their proportions matter, the
# actual speed of this machine is learnt from the files as they finish.
NOMINAL_S = {'rt/vu.c': 28.0,          # with the translated VU1 program: one very large function
             'rt/interp.c': 2.0, 'rt/support.c': 0.3, 'gen/fn_table.c': 1.0}
NOMINAL_S_PER_BYTE = 8.85e-6           # the recompiled code, by source size
WARM_UP = '(warm-up link)'


def build_in(iso, out, jobs, work):
    game = os.path.join(out, 'ror-game' + EXE)
    log = open(os.path.join(out, 'build.log'), 'w', encoding='utf-8')
    log.write('ror-build %s, kit %s, %s, %d jobs\n' % (kit_text('VERSION'), kit_text('KITHASH')[:16], sys.platform, jobs))

    env = {k: v for k, v in os.environ.items() if not k.startswith('PYTHON')}     # the bundled Python, nothing of the user's
    env['PYTHONNOUSERSITE'] = '1'
    env['ZIG_GLOBAL_CACHE_DIR'] = os.path.join(work, 'zig-cache')
    env['ZIG_LOCAL_CACHE_DIR'] = os.path.join(work, 'zig-cache')
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    zig = os.path.join(KIT, 'zig', 'zig' + EXE)
    python = sys.executable
    shown = [-1, '']

    def progress(pct, text):
        pct = max(int(pct), shown[0])                      # never backwards
        if (pct, text) != (shown[0], shown[1]):
            shown[0], shown[1] = pct, text
            say('PROGRESS %d 100 %s' % (pct, text))

    progress(0, 'Checking your disc')
    elf = read_elf_from_iso(iso)
    ident = identity(iso, elf)
    if ident['elf_sha256'] != ELF_SHA256:
        raise BuildError('this is not the European Rule of Rose disc (SLES-54218): its %s is a different version' % GAME_ELF)
    with open(os.path.join(work, GAME_ELF), 'wb') as f:
        f.write(elf)

    progress(1, 'Reading the game')
    run([python, '-B', os.path.join(KIT, 'recomp', 'gen.py'), GAME_ELF, 'gen'], work, env, log)
    progress(5, 'Reading the game')
    run([python, '-B', os.path.join(KIT, 'recomp', 'vu1extract.py'), GAME_ELF, 'vu1_micro.bin'], work, env, log)
    run([os.path.join(KIT, 'vugen' + EXE), 'gen', 'vu1_micro.bin'], work, env, log,
        stdout_path=os.path.join(work, 'gen', 'vu1_fast_gen.inc'))

    # The runtime sources that include generated headers are compiled here too; copied so that every path the
    # compiler sees is relative to the work folder.
    shutil.copytree(os.path.join(KIT, 'rt', 'src'), os.path.join(work, 'rt'))
    os.makedirs(os.path.join(work, 'obj'))
    sources = ['gen/' + n for n in os.listdir(os.path.join(work, 'gen')) if n.endswith('.c')]
    sources += ['rt/' + n for n in os.listdir(os.path.join(work, 'rt')) if n.endswith('.c')]
    nominal = {s: NOMINAL_S.get(s) or os.path.getsize(os.path.join(work, s)) * NOMINAL_S_PER_BYTE for s in sources}
    sources.sort(key=lambda s: (-nominal[s], s))           # the longest first: nothing big is left for the end
    workers = max(1, min(jobs, len(sources)))
    t_compile = time.monotonic()
    st = {'n': 0, 'nominal_done': 0.0, 'actual_done': 0.0, 'finished': False}

    def compile_progress():
        # this machine against the reference, once a fifth of the work says something about it
        speed = 1.0
        if st['nominal_done'] >= 0.2 * sum(nominal.values()):
            speed = min(max(st['actual_done'] / st['nominal_done'], 0.4), 4.0)
        # as long as the longest file, or as all of them shared among the workers, whichever is more
        # (the longest file is scaled less: the measured speed includes how much the other compilers slow each
        # other down, which touches a single long compile much less)
        expect = max(max(nominal.values()) * speed ** 0.5, sum(nominal.values()) * speed / workers, 1.0)
        frac = min((time.monotonic() - t_compile) / expect, 0.99)
        progress(6 + 88 * frac, 'Building the game (%d of %d)' % (st['n'], len(sources)))

    def ticker():
        while not st['finished']:
            with log_lock:
                if not st['finished']:
                    compile_progress()
            try:
                os.utime(os.path.join(work, 'pid'))         # "this build is alive" (remove_stale_work)
            except OSError:
                pass
            time.sleep(0.5)

    def compile_one(src):
        if src == WARM_UP:
            # The first link with a target makes the compiler build its C library and support routines, which
            # takes it a quarter of a minute on Windows. Linking an empty program now, beside the compiling,
            # gets that done (it is cached in the work folder) before the real link needs it.
            with open(os.path.join(work, 'warm.c'), 'w') as f:
                f.write('int main(void) { return 0; }\n')
            run([zig, 'cc'] + TARGET + ['-o', 'warm' + EXE, 'warm.c'] + LINK_LIBS + ['-s'], work, env, log)     # the real link's options: they are part of the cache key
            return None
        obj = 'obj/' + os.path.basename(src)[:-2] + '.o'
        t0 = time.monotonic()
        run([zig, 'cc'] + CFLAGS + ['-Irt', '-Igen', '-c', src, '-o', obj], work, env, log)
        with log_lock:
            st['n'] += 1
            st['nominal_done'] += nominal[src]
            st['actual_done'] += time.monotonic() - t0
            compile_progress()
        return obj

    threading.Thread(target=ticker, daemon=True).start()
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            futures = [pool.submit(compile_one, s) for s in [WARM_UP] + sources]
            try:
                objs = sorted(o for o in [f.result() for f in futures] if o)
            except BaseException:
                for f in futures:
                    f.cancel()
                raise
    finally:
        with log_lock:
            st['finished'] = True

    progress(95, 'Finishing')
    libdir = os.path.join(KIT, 'rt', 'lib')
    prebuilt = sorted(os.path.join(libdir, n) for n in os.listdir(libdir) if n.endswith(('.o', '.a')))
    with open(os.path.join(work, 'link.rsp'), 'w', encoding='utf-8') as f:      # a response file: the command line stays short
        for p in objs + prebuilt:
            f.write('"%s"\n' % p.replace('\\', '/'))
    run([zig, 'cc'] + TARGET + ['-o', 'ror-game' + EXE, '@link.rsp'] + LINK_LIBS + ['-s'], work, env, log)

    progress(99, 'Finishing')
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
