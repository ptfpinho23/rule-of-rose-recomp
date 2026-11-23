#!/usr/bin/env python3
"""irxdis.py [IRX] [-o OUT] — annotated disassembly of an IOP module (IRX).

Default input is build/irx/RXSNDDRV.IRX (extracted from the ISO on first use).
The listing marks function starts (jal targets / data code pointers), names
calls into import stubs (library + export index), and resolves lui/addiu,
lui/ori and lui/load pairs to absolute addresses (with the string if any).
Addresses are module-relative (the IRX is linked at 0).
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'recomp'))
sys.path.insert(0, ROOT)
from r5900 import decode, BRANCHES  # noqa: E402
import disasm  # noqa: E402

ISO = os.path.join(ROOT, 'Rule of Rose (Europe) (En,Fr,De,Es,It).iso')

EXPORTS = {
    'libsd': {4: 'sceSdInit', 5: 'sceSdSetParam', 6: 'sceSdGetParam', 7: 'sceSdSetSwitch',
              8: 'sceSdGetSwitch', 9: 'sceSdSetAddr', 10: 'sceSdGetAddr', 11: 'sceSdSetCoreAttr',
              12: 'sceSdGetCoreAttr', 13: 'sceSdNote2Pitch', 14: 'sceSdPitch2Note',
              15: 'sceSdProcBatch', 16: 'sceSdProcBatchEx', 17: 'sceSdVoiceTrans',
              18: 'sceSdBlockTrans', 19: 'sceSdVoiceTransStatus', 20: 'sceSdBlockTransStatus',
              21: 'sceSdSetTransCallback', 22: 'sceSdSetIRQCallback', 23: 'sceSdSetEffectAttr',
              24: 'sceSdGetEffectAttr', 25: 'sceSdClearEffectWorkArea',
              26: 'sceSdSetTransIntrHandler', 27: 'sceSdSetSpu2IntrHandler',
              28: 'sceSdGetTransIntrHandlerArgument', 29: 'sceSdGetSpu2IntrHandlerArgument',
              30: 'sceSdStopTrans', 31: 'sceSdCleanEffectWorkArea', 32: 'sceSdSetEffectMode',
              33: 'sceSdSetEffectModeParams'},
    'sifcmd': {4: 'sceSifInitCmd', 5: 'sceSifExitCmd', 6: 'sceSifGetSreg', 7: 'sceSifSetSreg',
               8: 'sceSifSetCmdBuffer', 9: 'sceSifSetSysCmdBuffer', 10: 'sceSifAddCmdHandler',
               11: 'sceSifRemoveCmdHandler', 12: 'sceSifSendCmd', 13: 'isceSifSendCmd',
               14: 'sceSifInitRpc', 15: 'sceSifBindRpc', 16: 'sceSifCallRpc',
               17: 'sceSifRegisterRpc', 18: 'sceSifCheckStatRpc', 19: 'sceSifSetRpcQueue',
               20: 'sceSifGetNextRequest', 21: 'sceSifExecRequest', 22: 'sceSifRpcLoop',
               23: 'sceSifGetOtherData', 24: 'sceSifRemoveRpc', 25: 'sceSifRemoveRpcQueue',
               28: 'sceSifSendCmdIntr', 29: 'isceSifSendCmdIntr'},
    'sifman': {4: 'sceSifDma2Init', 5: 'sceSifInit', 6: 'sceSifSetDChain', 7: 'sceSifSetDma',
               8: 'sceSifDmaStat', 9: 'sceSifSetOneDma', 29: 'sceSifCheckInit',
               32: 'sceSifSetDmaIntr'},
    'thbase': {4: 'CreateThread', 5: 'DeleteThread', 6: 'StartThread', 7: 'StartThreadArgs',
               8: 'ExitThread', 9: 'ExitDeleteThread', 10: 'TerminateThread',
               11: 'iTerminateThread', 12: 'DisableDispatchThread', 13: 'EnableDispatchThread',
               14: 'ChangeThreadPriority', 15: 'iChangeThreadPriority',
               16: 'RotateThreadReadyQueue', 17: 'iRotateThreadReadyQueue',
               18: 'ReleaseWaitThread', 19: 'iReleaseWaitThread', 20: 'GetThreadId',
               21: 'CheckThreadStack', 22: 'ReferThreadStatus', 23: 'iReferThreadStatus',
               24: 'SleepThread', 25: 'WakeupThread', 26: 'iWakeupThread',
               27: 'CancelWakeupThread', 28: 'iCancelWakeupThread', 29: 'SuspendThread',
               30: 'iSuspendThread', 31: 'ResumeThread', 32: 'iResumeThread', 33: 'DelayThread',
               34: 'GetSystemTime', 35: 'SetAlarm', 36: 'iSetAlarm', 37: 'CancelAlarm',
               38: 'iCancelAlarm', 39: 'USec2SysClock', 40: 'SysClock2USec',
               41: 'GetSystemStatusFlag'},
    'thevent': {4: 'CreateEventFlag', 5: 'DeleteEventFlag', 6: 'SetEventFlag',
                7: 'iSetEventFlag', 8: 'ClearEventFlag', 9: 'iClearEventFlag',
                10: 'WaitEventFlag', 11: 'PollEventFlag', 13: 'ReferEventFlagStatus'},
    'vblank': {4: 'WaitVblankStart', 5: 'WaitVblankEnd', 6: 'WaitVblank', 7: 'WaitNonVblank',
               8: 'RegisterVblankHandler', 9: 'ReleaseVblankHandler'},
    'sysmem': {4: 'AllocSysMemory', 5: 'FreeSysMemory', 6: 'QueryMemSize',
               7: 'QueryMaxFreeMemSize', 8: 'QueryTotalFreeMemSize', 9: 'QueryBlockTopAddress',
               10: 'QueryBlockSize', 14: 'Kprintf'},
    'intrman': {4: 'RegisterIntrHandler', 5: 'ReleaseIntrHandler', 6: 'EnableIntr',
                7: 'DisableIntr', 8: 'CpuDisableIntr', 9: 'CpuEnableIntr',
                17: 'CpuSuspendIntr', 18: 'CpuResumeIntr', 23: 'QueryIntrContext'},
    'loadcore': {6: 'RegisterLibraryEntries', 7: 'ReleaseLibraryEntries'},
    'stdio': {4: 'printf'},
    'sysclib': {8: 'look_ctype_table', 10: 'memchr', 11: 'memcmp', 12: 'memcpy', 13: 'memmove',
                14: 'memset', 15: 'bcmp', 16: 'bcopy', 17: 'bzero', 19: 'sprintf', 20: 'strcat',
                21: 'strchr', 22: 'strcmp', 23: 'strcpy', 27: 'strlen', 29: 'strncmp',
                30: 'strncpy'},
}


class Irx:
    def __init__(self, path):
        if not os.path.exists(path):
            from rorcore.iso9660 import IsoImage
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, 'wb').write(IsoImage(ISO).read('/MODULES/' + os.path.basename(path)))
        self.d = d = open(path, 'rb').read()
        (_entry, _phoff, shoff, _f, _e, _phentsz, _phnum, shentsz, shnum,
         shstrndx) = struct.unpack_from('<IIIIHHHHHH', d, 0x18)
        raw = [struct.unpack_from('<IIIIIIIIII', d, shoff + i * shentsz) for i in range(shnum)]
        so = raw[shstrndx][4]
        self.sec = {}
        for r in raw:
            nm = d[so + r[0]:d.index(b'\0', so + r[0])].decode()
            self.sec[nm] = dict(type=r[1], addr=r[3], off=r[4], size=r[5])
        t = self.sec['.text']
        self.base_off = t['off'] - t['addr']
        self.text_end = t['addr'] + t['size']
        self.load_end = self.sec['.data']['addr'] + self.sec['.data']['size']
        self.bss_end = self.sec['.bss']['addr'] + self.sec['.bss']['size']
        # relocations: set of addresses carrying an absolute (R_MIPS_32) pointer
        self.ptr_relocs = set()
        for nm in ('.rel.text', '.rel.rodata', '.rel.data'):
            s = self.sec.get(nm)
            if not s:
                continue
            for o in range(s['off'], s['off'] + s['size'], 8):
                a, info = struct.unpack_from('<II', d, o)
                if info & 0xFF == 2:
                    self.ptr_relocs.add(a)
        # import stubs
        self.stubs = {}
        a = 0
        while a < self.text_end:
            if self.word(a) == 0x41E00000 and self.word(a + 4) == 0:
                lib = self.bytes(a + 12, 8).split(b'\0')[0].decode()
                b = a + 20
                while self.word(b) == 0x03E00008:
                    idx = self.word(b + 4) & 0xFFFF
                    self.stubs[b] = f'{lib}.{EXPORTS.get(lib, {}).get(idx, idx)}'
                    b += 8
                a = b
            else:
                a += 4

    def bytes(self, a, n):
        if a >= self.load_end:
            return b'\0' * n
        return self.d[self.base_off + a:self.base_off + a + n]

    def word(self, a):
        return struct.unpack('<I', self.bytes(a, 4))[0]

    def cstr(self, a):
        if not (self.text_end <= a < self.load_end):
            return None
        e = self.d.index(b'\0', self.base_off + a)
        s = self.d[self.base_off + a:e]
        if len(s) >= 3 and all(32 <= c < 127 or c in (9, 10) for c in s):
            return s.decode()
        return None


def listing(x, out):
    funcs = set()
    for a in range(0, x.text_end, 4):
        i = decode(x.word(a), a)
        if i.mn == 'jal':
            funcs.add(i.target)
    for a in x.ptr_relocs:
        v = x.word(a)
        if v < x.text_end and v % 4 == 0:
            funcs.add(v)
    last = {}
    for a in range(0, x.text_end, 4):
        if a in x.stubs:
            out.write(f'\n{a:05x}: IMPORT {x.stubs[a]}\n')
            continue
        if a - 4 in x.stubs:
            continue
        if a in funcs:
            out.write(f'\n; ---------------- func_{a:05x}\n')
            last = {}
        w = x.word(a)
        i = decode(w, a)
        text = disasm.fmt(i)
        note = ''
        if i.mn in ('jal', 'j') and i.target in x.stubs:
            note = x.stubs[i.target]
        elif i.mn == 'lui':
            last[i.rt] = i.uimm << 16
        elif i.mn in ('addiu', 'ori') and i.rs in last:
            v = (last[i.rs] + i.imm) & 0xFFFFFFFF if i.mn == 'addiu' else last[i.rs] | i.uimm
            s = x.cstr(v)
            note = f'= 0x{v:x}' + (f' {s!r}' if s else '') + (' (code)' if v in funcs else '')
            if i.rt != i.rs:
                last.pop(i.rt, None)
            else:
                last.pop(i.rs)
        elif i.mn in disasm.LOADS and i.rs in last:
            v = (last[i.rs] + i.imm) & 0xFFFFFFFF
            note = f'[0x{v:x}]'
        out.write(f'{a:05x}: {w:08x}  {text:36s}{" ; " + note if note else ""}\n')
    # data: pointer tables
    out.write('\n; ---------------- relocated pointers in data\n')
    for a in sorted(p for p in x.ptr_relocs if p >= x.text_end):
        v = x.word(a)
        s = x.cstr(v)
        out.write(f'{a:05x}: -> 0x{v:x}' + (' (code)' if v in funcs else '') + (f' {s!r}' if s else '') + '\n')


def main():
    args = sys.argv[1:]
    out = sys.stdout
    if '-o' in args:
        k = args.index('-o')
        out = open(args[k + 1], 'w')
        del args[k:k + 2]
    path = args[0] if args else os.path.join(ROOT, 'build/irx/RXSNDDRV.IRX')
    listing(Irx(path), out)


if __name__ == '__main__':
    main()
