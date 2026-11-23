# RXSNDDRV.IRX — protocol and data formats

Reverse-engineered from `MODULES/RXSNDDRV.IRX` ("Sound driver version 1.00",
74,005 bytes, IOP MIPS-I relocatable, uses LIBSD) and from the game's EE
client library ("rxsnd", `SLES_542.18` 0x239cf8-0x241e80).

Address notation: `IRX 0x1234` is a module-relative address in the driver
(text 0-0xd0f0, rodata 0xd0f0, data 0xdd60, bss 0xe4f0-0x38970; file offset =
address + 0xb0). `EE 0x241b40` is an address in the game executable.

Evidence tags: **[V]** verified by reading the code at the address given;
**[I]** inferred (from naming, debug strings, data, or behaviour that follows
from verified code but was not traced end to end); **[?]** unknown.

Tools used (all under `recomp/tools/`):
`irxdis.py` (annotated IRX listing: import names, resolved address pairs, jump
tables), `eelist.py` (annotated EE listing), `ee_calls.py` (callers of an EE
function with constant arguments).

Nothing in this document has been exercised against the running game yet: the
boot currently stops before the sound layer does anything except allocate IOP
heap. Everything is static analysis.

---------------------------------------------------------------------------

## 1. Transport

### 1.1 RPC server

* SIF RPC server id **0x0000D000**, registered by the driver's RPC thread
  (priority 12) with `sceSifRegisterRpc` and a receive buffer at IRX 0xf504
  **[V IRX 0xbc4-0xc10]**.
* Handler **[V IRX 0xc24]**: `result = dispatch(fno, out = 0x10508, in = data)`,
  stores `result` at 0x10504 and returns 0x10504. So for every call:

  ```
  fn   = command number
  send = argument block ("in")
  recv = { s32 result; u8 out[...]; }
  ```

* EE side **[V EE 0x241b40]**: `rxsnd_cmd(cmd, out_ptr, out_size, in_ptr, in_size)`
  copies the argument words into a per-thread send buffer
  (0x7f3200 + 0x400 * thread_index, thread_index = `[0x2f1328]`, 0 or 1), calls
  `sceSifCallRpc(client 0x7f4200, cmd, mode 1 (no wait), send, (in_size+63)&~63,
  recv = 0x7f3a00 + 0x400*idx, (out_size+67)&~63, end_func 0x2419f0)` through
  EE 0x241a20, spins on `sceSifCheckStatRpc` until done, copies `out_size`
  bytes from recv+4 to `out_ptr`, and returns recv word 0 (`result`).
  Binding: EE 0x241ae8 loops on bind(0xD000) until it succeeds.

### 1.2 Batched commands (command 512)

Most commands are not sent one by one. The EE keeps a queue
**[V EE 0x241cd0-0x241e78]**:

| EE function | role |
|---|---|
| 0x241cd0 `queue(cmd, out_size, in_size)` | append header `{cmd, out_size, in_size}` at 0x7f4240+pos (flushes first if pos+in_size+12 > 4096 or out_pos+out_size > 4096) |
| 0x241d80 | pointer to this command's in-data (header + 12) |
| 0x241d68 | pointer where this command's out-data will be after the flush (0x7f5240 + out_pos; out_pos starts at 4) |
| 0x241d98 | commit: pos += in_size + 12, out_pos += out_size |
| 0x241de0 `flush()` | if pos > 0: append a zero header, RPC **cmd 512** with send = the queue (pos+12 bytes), recv = 0x7f5240 (out_pos bytes), then pos = 0, out_pos = 4 |

Driver side **[V IRX 0xa7c-0xac0]**: command 512 walks records

```
struct { u32 cmd; u32 out_size; u32 in_size; u8 in[in_size]; }   (packed, no padding)
```

until `cmd == 0`, calling the dispatcher recursively with `in` = record+12 and
`out` advancing by each record's `out_size`. The per-command return values are
**discarded**; the RPC result of command 512 is 0. Out data of record *n*
therefore lands at recv + 4 + sum(out_size of earlier records).

`in_size` is not rounded by the EE; records are byte-packed (all callers use
multiples of 4).

### 1.3 Other data paths

* **EE -> IOP memory**: plain `sceSifSetDma` to an IOP address, wrapped by
  EE 0x23a6e0 `iop_write(iop_dst, ee_src, size)` **[V]**. For aligned
  requests (src 16-aligned, size multiple of 16, dst 4-aligned) it is one
  `sceSifSetDma {src, dst, size, 0}` followed by a busy loop on
  `sceSifDmaStat(id) >= 0`. Unaligned heads/tails go through a 32-byte bounce
  buffer on the IOP (addresses in the table at EE 0x7e0a40, one per EE
  thread) and command 35 (IOP memmove). Used for XVAB headers, XSEQ data and
  all stream data.
* **EE -> SPU RAM / SPU RAM -> EE**: transfer types 3 and 4 of command 64; the
  driver pulls EE memory with `sceSifGetOtherData` (IRX 0x1618) or pushes with
  `sceSifSetDma` (IRX 0x1580). See section 4.
* **IOP -> EE notifications: none.** The driver never calls `sceSifSendCmd`
  and never initiates an RPC (its import table has only `sceSifInitRpc`,
  `sceSifRegisterRpc`, `sceSifSetRpcQueue`, `sceSifRpcLoop`,
  `sceSifGetOtherData`, `sceSifSetDma`, `sceSifDmaStat`) **[V import stubs
  IRX 0xcfac-0xcff8]**. All status is polled by the EE.

### 1.4 Status polling

Once per game frame the EE runs **[V EE 0x239dd8]**:

```
flush();
queue(114, 260, 0)   -> BGM status      -> EE 0x23db00
queue(131, 260, 0)   -> SE status       -> EE 0x23e8a0
queue(179, 388, 0)   -> PCM status      -> EE 0x23f8c8
queue(163, 772, 0)   -> ADPCM status    -> EE 0x241610
flush();
```

The four consumers are described with their subsystems. "Stream needs more
data", "stream position", "slot finished/closed" are all derived from these
blocks; there are no callbacks from the IOP.

### 1.5 Driver timing model (double buffering)

The driver's worker thread **[V IRX 0x29c-0x358]** (priority 35, created by
command 16 and once at module start, IRX 0x100) loops:

```
while (!terminate_request) {
    WaitVblankEnd();
    flip ^= 1;                 /* 0xe4f4, under CpuSuspendIntr */
    cfg_tick();      /* 0x1680 */   core_tick();   /* 0x19e0 */
    effect_tick();   /* 0x1ca0 */   bgm_tick();    /* 0x5e60 */
    se_tick();       /* 0x8878 */   note_tick();   /* 0x93c0 */
    adpcm_tick();    /* 0xb1dc */   voice_tick();  /* 0x35c0 */
    pcm_tick();      /* 0xb5ac */
}
terminated = 1;                /* 0xe4fc */
```

Almost every "set" command only records a request in one of two request sets
per object; RPC handlers write set `flip` (IRX 0x268), the tick reads set
`!flip` after flipping (IRX 0x254), i.e. **a request takes effect at the next
vblank tick**, and a status command in the same batch still sees the old
state. PAL: one tick every 20 ms.

---------------------------------------------------------------------------

## 2. Command summary

`in` = argument block, `bN` = u8 at offset N, `hN` = s16, `wN` = u32.
"result" is recv word 0 and is only visible for direct (non-batched) calls.
"EE use" says how the game sends it: D = direct `rxsnd_cmd`, Q = queued in a
batch, - = never sent by this game.

| cmd | hex | name | in | result / out | IRX | EE use |
|---:|---|---|---|---|---|---|
| 16 | 0x10 | start worker thread | - | 1 ok, 0 failed | 0x190 | - (module start does it) |
| 17 | 0x11 | stop worker thread | - | "terminated" flag as it was at the call | 0x238 | D (shutdown) |
| 18 | 0x12 | version | - | u16 at 0xdd64 | 0x27c | - |
| 32 | 0x20 | AllocSysMemory | w0 flags, w4 size, w8 addr | address or 0 | 0x608 | D |
| 33 | 0x21 | FreeSysMemory | w0 addr | 0 | 0x630 | D |
| 34 | 0x22 | memory query | w0: 0 QueryMemSize, 1 QueryMaxFreeMemSize, 2 QueryTotalFreeMemSize | value | 0x650 | - (wrappers exist, no callers) |
| 35 | 0x23 | IOP memmove | w0 dst, w4 src, w8 size | 0 | 0x6d0 | D (unaligned IOP writes) |
| 48 | 0x30 | driver config | w0 code, w4 value | 0 | 0x18b0 | D |
| 49 | 0x31 | core settings | w0 core, w4 code, +8 value | 0 | 0x1bdc | - (wrappers exist, no callers) |
| 50 | 0x32 | effect (reverb) | w0 core, w4 code, +8 value | 0 | 0x21e0 | Q |
| 64 | 0x40 | queue SPU transfer | b0 type, w4 a, w8 b, w12 size | transfer id, -1 if queue full | 0x1124 | D |
| 65 | 0x41 | transfer busy? | w0 id | 1 queued/in progress, 0 done | 0x127c | D |
| 80 | 0x50 | open XVAB | w0 id, w4 IOP header addr, w8 SPU body addr | see 6 | 0x2330 | D |
| 81 | 0x51 | close XVAB | w0 id | see 6 | 0x28b8 | D |
| 96 | 0x60 | open XSEQ | w0 id, w4 IOP addr | see 7 | 0x2e00 | - |
| 97 | 0x61 | close XSEQ | w0 id | see 7 | 0x31d4 | - |
| 99 | 0x63 | XSEQ bank binding | w0 id, w4 index, w8 XVAB id | 0 | 0x3310 | - |
| 112 | 0x70 | BGM play | w0 BGM slot, w4 XSEQ id | see 8 | 0x4680 | - |
| 113 | 0x71 | BGM slot control | w0 slot, w4 code, +8 value | 0 | 0x59ec | - (only reachable for a playing BGM) |
| 114 | 0x72 | BGM status | - | out 260 bytes | 0x5b68 | Q (every frame) |
| 115 | 0x73 | BGM channel control | b0, b1, +4 {code, value} | 0 | 0x5c10 | - |
| 116 | 0x74 | BGM channel control 2 | b0, b1, +4 {code, value} | 0 | 0x5d20 | - |
| 128 | 0x80 | SE play | b0..b3 | see 9 | 0x84b0 | Q |
| 129 | 0x81 | SE play single note | b0..b3 | see 9 | 0x8658 | - |
| 130 | 0x82 | SE slot control | w0 slot, w4 code, +8 value | 0 | 0x6bcc | Q |
| 131 | 0x83 | SE status | - | out 260 bytes | 0x8800 | Q (every frame) |
| 144 | 0x90 | voice status A | - | out 192 bytes | 0x4384 | - |
| 145 | 0x91 | voice control | w0, w4 | 0 | 0x451c | - |
| 146 | 0x92 | voice status B | - | out 192 bytes | 0x4540 | - |
| 147 | 0x93 | voice status C | w0 | out 192 bytes | 0x43b4 | - |
| 160 | 0xa0 | ADPCM stream open | see 10 | 1 ok, 0 slot busy | 0x9eb0 | Q |
| 161 | 0xa1 | ADPCM data added | b0 slot, b1 end flag, w4 bytes | bytes requested | 0xad94 | Q |
| 162 | 0xa2 | ADPCM slot control | w0 slot, w4 code, +8 value | 0 | 0xaea0 | Q |
| 163 | 0xa3 | ADPCM status | - | out 772 bytes | 0xb160 | Q (every frame) |
| 164 | 0xa4 | ADPCM channel control | h0 slot, h2 channel, w4 code, +8 value | 0 | 0xb01c | Q |
| 176 | 0xb0 | PCM stream open | b0 slot, b1 mode, w4 IOP buffer, w8 blocks | 1 ok, 0 slot busy | 0xc944 | - (no caller of the EE open) |
| 177 | 0xb1 | PCM blocks added | b0 slot, b1 count, +4 u32 tag[count] | 0 | 0xcabc | - |
| 178 | 0xb2 | PCM slot control | w0 slot, w4 code, +8 value | 0 | 0xcc28 | - |
| 179 | 0xb3 | PCM status | - | out 388 bytes | 0xcd58 | Q (every frame) |
| 180 | 0xb4 | (not in this driver) | w0 | ignored | 0xac4 | wrapper EE 0x23eb90 |
| 512 | 0x200 | batch | records | 0 | 0xa7c | flush |

The dispatcher is IRX 0x360-0xb04 (a compare tree, every case **[V]**).
Any other number falls to IRX 0xac4: prints
`snddrv : invalide RPC command '%d'` when debug bit 0 is set and returns 0.
Command 180 is such a number: the EE has a function that queues it
(EE 0x23eb90, one word argument) but this driver build has no case for it.

"EE use = -" was established by searching the whole EE text for `jal` to the
wrapper and for data references to it (`ee_calls.py`, `xref.py`). What the
game actually uses: 17, 32, 33, 35, 48 (code 2 only), 50 (type and depth),
64 (type 3 only), 65, 80, 81, 128, 130, 131, 160-164, the four status
commands and 512. **BGM/XSEQ and PCM streams are dead code in this game**
**[V: no callers of EE 0x23c990, 0x23d1f8, 0x23ebd0]**; the disc has no
`XSEQ` data **[I: whole-image search]**.

How sections 3-9 were produced: sections 1, 2, 4, 10, 11 and the dispatcher
were read directly. Sections 3 and 5-9 come from three delegated read-throughs
of the listed address ranges; from those I re-read myself the SE play
defaults (IRX 0x84b0), SE note-on/key-off opcodes (IRX 0x741c-0x765c,
0x8a5c-0x8bc8), the voice setters and allocator (IRX 0x3b80-0x4380) and the
"no callers" claims. The XVAB layout in section 6 is additionally confirmed
by `recomp/tools/rxsnd_test.sh`, which plays an SE from the real `0000.SE`
through the implementation. Everything else in those sections carries the
address it was read from but was not double-checked.

---------------------------------------------------------------------------

## 3. Memory, configuration, core settings, effects

### 3.1 IOP memory (32, 33, 34, 35) **[V IRX 0x608-0x778]**

* 32: `AllocSysMemory(w0 flags, w4 size, w8 addr)` with interrupts suspended.
  The EE only sends flags 0 with size rounded up to 64 (EE 0x23a4e0); result 0
  makes the EE print "can't allocate IOP heap" and fail the operation.
  The EE library's init (EE 0x23ac50) allocates two 32-byte bounce buffers,
  which are the two calls seen at boot.
* 33: `FreeSysMemory(w0)`; result always 0.
* 34: w0 = 0/1/2 as in the table; other values return 0.
* 35: overlap-safe byte copy of w8 bytes from IOP w4 to IOP w0.

The returned addresses must be real IOP RAM addresses: the EE writes to them
with `sceSifSetDma` and later passes them back in commands 80, 96, 160, 176.

### 3.2 Driver config (48) **[V IRX 0x18b0, tick 0x1680, defaults 0x1814]**

`{w0 code, w4 value}`; applied at the next tick.

| code | value | meaning | default |
|---|---|---|---|
| 0 | u32 | debug print mask (bit 0 errors, bit 1 warnings, bit 2 info) [I for bit meaning] | 7 |
| 1 | u32 | video rate used for timing: SE tempo, BGM tempo, ADPCM start threshold (IRX 0x1880 readers) | 60 |
| 2 | u32 | 1 stereo / 0 mono (voices: L = R = (|L|+|R|)/2, IRX 0x38a4; PCM mixers likewise) | 1 |
| 3, 4, 5 | u8 | digital-out bits -> `sceSdSetCoreAttr(10, ...)` (IRX 0x17f0) [I: entry 10 = SPDIF mode] | 1, 1, 1 |

EE: only code 2 is sent by the game (EE 0x239eb8 from 0x1cb110, value 0 or 1).
The EE wrapper for code 1 would send 50 or 60 but is never called, so the
driver runs with rate 60 on a 50 Hz console (SE with tempo 60 advance one
delta unit per vblank regardless).

### 3.3 Core settings (49) **[V IRX 0x1bdc, tick 0x19e0, init 0x1b5c]**

`{w0 core, w4 sub, +8 value}`: sub 0 = MMIX mask (`sceSdSetParam(core|0x800, v & 0xFFF)`),
sub 1 = master volume `{s16 L, s16 R}` (`core|0x980`, `core|0xA80`); other subs
ignored. Init batch at 0xdd70: MMIX = 0xFCC and MVOLL/R = 0x3FFF on both cores.

### 3.4 Effects (50) **[V IRX 0x21e0, tick 0x1ca0, init 0x20c4, table 0xdda0]**

`{w0 core, w4 sub, +8 value}`:

| sub | value | effect |
|---|---|---|
| 0 | w8 type 0..9 | reverb type; restarts the effect (see below) |
| 1 | w8 | effect area end address EEA (default core 0 0x1FFFFF, core 1 0x1DFFFF); restarts |
| 2 | h8 L, h10 R | target depth; the current depth moves 8 per tick towards it |
| 3 | h8 | feedback |
| 4 | h8 | delay |

Restart: `sceSdSetCoreAttr(core|2, 0)`, `sceSdSetAddr(core|0x1D00, EEA)`,
zero `sceSdSetEffectAttr`, then an internal type-5 transfer clears the work
area `[EEA+1-size, EEA]` (size from the table: 0x80, 0x26C0, 0x1F40, 0x4840,
0x6FE0, 0xADE0, 0xF6C0, 0x18040, 0x18040, 0x3C00, rounded up to 64); when the
clear completes the depth restarts from 0. Running state: on any change
`sceSdSetEffectAttr{mode = type, depth = clamp(depth << 8), delay, feedback
clamped 0..127}` and `sceSdSetCoreAttr(core|2, 1)`.

EE: type (EE 0x23bd80) and depth (EE 0x23be20), each for both cores, queued.

---------------------------------------------------------------------------

## 4. SPU RAM transfers (64, 65) **[V IRX 0xc60-0x1570]**

A "DMA management" thread (priority 13) serialises `sceSdVoiceTrans` on
core 0. Two queues of 256 entries of 32 bytes
`{id, type, a, b, size, callback, cb_arg, cb_arg2}`; queue 0 (internal:
ADPCM stream feeding) is served before queue 1 (command 64 and the effect
clear). `sceSdSetTransIntrHandler(0, 0xed4)` advances multi-chunk transfers
and pops the queue.

Command 64 `{b0 type, w4 a, w8 b, w12 size}` -> transfer id (a counter
starting at 0, shared by both queues), or -1 if the queue is full:

| type | a | b | action | IRX |
|---|---|---|---|---|
| 1 | SPU addr | IOP addr | IOP RAM -> SPU RAM | 0xd7c |
| 2 | IOP addr | SPU addr | SPU RAM -> IOP RAM | 0xd98 |
| 3 | SPU addr | EE addr | EE RAM -> SPU RAM, in chunks of at most 0x4000 through the IOP buffer at 0x15540 (`sceSifGetOtherData`, then VoiceTrans write) | 0xdb4 |
| 4 | EE addr | SPU addr | SPU RAM -> EE RAM (VoiceTrans read, then `sceSifSetDma`) | 0xe2c, 0xf54 |
| 5 | SPU addr | - | zero fill in 4096-byte chunks | 0xe6c |

Command 65 `{w0 id}` -> 1 while an entry with that id is still in a queue,
0 otherwise (IRX 0x127c).

EE: only type 3 is used, for XVAB bodies (EE 0x23b048: `{b0 = 3, b1 = 1,
w4 = spu, w8 = ee_ptr, w12 = size}`; the caller retries while the id is
negative). Completion: EE 0x23b1b0 polls command 65 until it returns 0.

---------------------------------------------------------------------------

## 5. Voice manager (144-147) **[V IRX 0x3310-0x45b0 unless noted]**

48 voices: 0..23 on core 0, 24..47 on core 1. There is no command to play a
voice directly; SE notes, BGM notes and ADPCM stream channels allocate
voices through IRX 0x3b80 and set them through IRX 0x409c-0x4208. All LIBSD
voice traffic is issued by the voice tick (IRX 0x35c0), once per vblank:

1. `sceSdGetSwitch` of VMIXL/VMIXR/VMIXEL/VMIXER/NON/PMON per core.
2. Per voice: `sceSdGetParam(ENVX)`; then for allocated voices apply dirty
   state: `sceSdSetAddr(SSA 0x2040)`, `sceSdSetParam(ADSR1 0x300, ADSR2
   0x400)`, `sceSdSetAddr(LSAX 0x2140)`, mix switch bits, `VOLL/VOLR =
   clamp(vol * scale >> 7, +-0x3FFF) & 0x7FFF`, `PITCH 0x200`.
3. `sceSdSetSwitch` of the six mix masks.
4. Key on `0x1500` for voices keyed on this tick, key off `0x1600` for
   voices keyed off this tick.

Voice end is detected **only through ENVX**: a voice allocated with the
"auto" flag is released when ENVX reads 0 on 9 consecutive ticks (IRX
0x39a8-0x39f0); its owner's callback is then called. ENDX is never read.
A voice stopped by its owner fades (scale -16 per tick) and is freed when
the scale reaches 0.

Allocation `0x3b80(voice or -1, priority, auto, group, owner, callback, arg)`:
an `owner` > 0 first stops every voice with the same owner id (reason 1 to
its callback); group 0 = any voice, 1 = core 0 only, 2 = core 1 only;
round-robin over voices that are enabled and not owned; if none, steal the
lowest priority (ties: the oldest) and call its callback with reason 1.

Commands (out = 48 u32 in voice order; all results 0):

| cmd | in | out |
|---|---|---|
| 144 | - | last ENVX of each voice |
| 146 | - | state flags: 1 allocated, 2 owned, 4 keyed on, 8 fading, 0x10 auto-release |
| 147 | w0 selector | 0 ENVX; 1 `(volL>>7)<<16 \| (volR>>7)&0xFFFF`; 2 pitch; 3 mix bits (1 dry, 2 effect, 4 noise, 8 pitch-mod); 4 priority; 5 `ADSR1<<16 \| ADSR2`; >= 6 leaves out untouched |
| 145 | w0 voice, w4 value | enable flag of the voice (0 removes it from allocation); no range check in the driver |

None of these is called by the game.

---------------------------------------------------------------------------

## 6. XVAB sample banks (80, 81)

### 6.1 Protocol

EE open, EE 0x23c0e0 `(id = -1, file, 0, 0)` from the game at 0x18110c **[V]**:

1. pick an EE slot (256 slots; id < 0 = first free);
2. check `file[0] == 'XVAB'`; header size H = `file[8]` (+16 if version
   `file[4]` >= 0x200); body = `file + align16(H)`, body size = HEAD+4;
3. command 32 (flags 0, H rounded up to 64) -> IOP address;
4. EE-side SPU heap allocation of the body size (section 6.3);
5. `iop_write(iop, file, H)` (SIF DMA into IOP RAM);
6. command 80 `{id, iop_addr, spu_addr}` -> id, or < 0: both allocations freed;
7. command 64 type 3 `{spu_addr, body, body_size}`; the game then polls
   command 65 until 0 (EE 0x23c648 with wait = 1).

Close, EE 0x23c4d0: command 81 `{id}`; result 1 = still in use, the EE spins
and retries; result 0 = closed, the EE then frees IOP memory (command 33)
and its SPU block.

Driver **[V IRX 0x2330, 0x28b8]**: 128 slots of 32 bytes at 0x19680
`{header ptr, TGIF, SEIF, VELT, PANT, LFOT pointers, SPU base, refcount}`.
Command 80: id < 0 = first free; an id >= 0 is not range-checked. Returns the
id, or -1 (slot in use, no free slot, bad magic). The driver rewrites the
header in place (file offsets -> pointers) for versions below 0x200.
Command 81: 1 if the slot is not open or its refcount is > 0 (a sounding
note or an SE slot holds a reference), else un-relocates, clears the slot
and returns 0.

### 6.2 File format (`/DATA/SND/XV/*.SE`, 34 files, all version 0x101)

One XVAB header followed by one ADPCM body; file size = align16(H) + body
size **[V on all 34 files]**.

```
+0   'XVAB'
+4   u32 version (0x101)
+8   u32 header size H
+12  chunks { char tag[4]; u32 size; u8 data[size]; } back to back up to H
     (version >= 0x200: chunks from +16 with 16-byte headers {tag, ?, size, ?};
      read from IRX 0x2da0 only, no sample on the disc)
body raw SPU ADPCM (16-byte blocks) at (H + 15) & ~15
```

Chunk order on disc: HEAD, TGIF, TGPG, TGSP, SEIF, SEGP, SERQ, SESQ, VELT,
PANT, LFOT. Offsets inside chunks are **file-relative**.

* **HEAD** (data at file 0x14): b0 "relocated" flag (0 on disc), b1 = 1 body
  follows, +4 u32 body size, +8/+12/+16/+20/+24 offsets of the TGIF, SEIF,
  VELT, PANT, LFOT data.
* **TGIF**: u16 program count, +4 u32 offset[count] to program headers.
* **TGPG** program header (8 bytes): b0 = 1, b1 = 0x20, u16 tone count,
  u32 offset of the tone array.
* **TGSP** tone (36 bytes) **[V IRX 0x9080-0x9260, 0x93c0-0x9890]**:

  | off | field |
  |---|---|
  | 0 | flags: 1 dry mix, 2 effect mix, 4 noise, 0x40 fixed pitch |
  | 1 | voice priority (10 on disc) |
  | 2 | exclusive group: nonzero -> owner id `xvab << 16 \| value` |
  | 3 | voice group 0..2 |
  | 4, 5 | key range low, high (inclusive) |
  | 6, 7 | root note, root fine |
  | 8, 9 | VELT curve index, PANT curve index |
  | 10, 11 | s8 volume L, R |
  | 12 | s16 pitch scale, 4096 = 48 kHz (0x0EB3 = 44.1 kHz on disc) |
  | 16, 18 | u16 ADSR1, ADSR2 (0x80FF / 0x5FC0 one-shot, 0x5FCB looped) |
  | 20 | u32 sample offset in the body (SSA = SPU base + offset) |
  | 24-32 | LFO table indices, depths, rate indices (vol/pan/pitch) [I] |
  | 33, 34 | pitch-bend range down, up (semitones) |

* **SEIF**: u16 group count, +4 u32 offset[]. **SEGP** group: u16 count,
  +4 u32 offset[]. **SERQ** entry (8 bytes): u8 volume L, u8 volume R
  (128 = unity), u16 flags (low 10 bits tempo, 0 = 60; 0x1000 / 0x2000
  negate L / R; 0xC000 voice group 0, 0x4000 group 1, 0x8000 group 2),
  u32 offset of the sequence in **SESQ** (section 9.3). On disc every entry
  is `80 80 00 00`.
* **VELT**: u32 count, 128-byte velocity curves. **PANT**: u32 count,
  256-byte pan curves (L gains, then R gains). **LFOT**: u32 count, 128-byte
  shapes (count 0 on disc).

Sample end: the SPU plays from SSA until a block whose flag byte (byte 1)
has bit 0; with bit 1 also set the sample loops (51 of 1078 tones), without
it the voice is muted, ENVX drops to 0 and the driver's 9-tick rule frees
the voice.

### 6.3 EE-side SPU heap **[V EE 0x23b2a8-0x23bd80]**

SPU addresses are chosen by the EE: range 0x5010-0x1C0000, blocks rounded
up to 64, first fit from the bottom, so the first bank body is at 0x5010.
The reverb work areas sit above (ending at 0x1DFFFF and 0x1FFFFF).

---------------------------------------------------------------------------

## 7. XSEQ sequences (96, 97, 99) — unused by the game

**[V IRX 0x2e00-0x3310 by delegated read; EE wrappers have no callers]**

* 96 `{w0 id, w4 iop_addr}`: 32 slots; id < 0 = first free. Returns the id,
  or -1 (slot used, none free, bad magic, no `HEAD`/`MIDI` chunk).
* 97 `{w0 id}`: 0 closed; 1 not closed (in use by a BGM, or not open).
* 99 `{w0 id, w4 index 0..7, w8 XVAB id}`: bank table of the sequence; a
  MIDI bank-select value indexes it to find the XVAB for note-ons.

Container as parsed by the driver (never seen on disc) **[I]**: `'XSEQ'`,
u32 version, u32 size; chunks as in XVAB. `HEAD` data: u8 track count, u8
relocated flag, s16 PPQN, u32 track offset[]; each track header's second
word is the offset of its event data; events are MIDI (status with running
status, variable-length delta **after** each event).

## 8. BGM (112-116) — unused by the game

**[V IRX 0x45b0-0x6ab0 by delegated read]**

* 112 `{w0 BGM slot (0..31), w4 XSEQ id}` -> 1 started, 0 failed (slot in
  use, XSEQ not open, out of tracks).
* 113 `{w0 slot, w4 code, +8 value}`, ignored unless the slot is in use:
  0 stop (b8: 1 = key off notes, 2 = cut), 1 pause (0 resume, 1 pause with
  key off, 2 pause with cut), 2 effect switch, 3 volume `{h8 L, h10 R}`
  (128 = unity), 4 pitch scale (4096 = 1.0), 5 tempo scale (4096 = 1.0),
  6 seek (w8 = byte offset in the track data).
* 115 `{b0 slot, b1 MIDI channel, w4 code = 3, h8 L, h10 R}` channel volume.
* 116 `{b0 slot, b1 track, w4 code = 3, h8 L, h10 R}` track volume.
* 114 status, out 260 bytes: for the i-th active slot (newest first)
  `out[4i] = slot`, `out[4i+1] = flags` (1 used, 2 playing, 4 finished,
  8 paused), `out[128+4i]` s32 = byte position in the first track;
  `out[256]` = count. The EE (0x23db00) stops a finished slot itself
  (113 code 0 value 2) and treats a slot as free once it is no longer listed.

Sequencer: per tick each track accumulates `acc -= D`, running events while
`acc <= 0` with `acc += delta * K`; `K = tempo_us * 60 / ((PPQN * 1000000) >> 12)`,
`D = rate * tempo_scale / 60`. Loops: NRPN 0 / 2560 = loop start (data entry
= repeat count, 0 = forever), NRPN 1 / 3840 = loop end. Meta FF 2F ends a
track; FF 51 sets the tempo.

---------------------------------------------------------------------------

## 9. SE (128-131)

### 9.1 Commands

* **128** `{b0 SE slot 0..63, b1 XVAB id, b2 group, b3 index}` play the SE
  sequence (group, index) of the bank; **129** `{b0 slot, b1 XVAB id,
  b2 program, b3 key}` play one note. Result 1, or 0 if the slot is already
  allocated **[V IRX 0x84b0, 0x8658]**. Synchronously: slot flags = 3,
  tempo 60, volumes 128, pitch 4096, a "start" request, and the slot is
  appended to the **tail** of the active list (0x2cde0). The lookup of the
  SE and the first notes happen at the next tick.
  The slot number is chosen by the EE (free list of 64 ids, LIFO, first id
  handed out is 63) **[V EE 0x23e838, 0x23dd90]**.
* **130** `{w0 slot, w4 code, +8 value}` **[V IRX 0x6bcc, applied at
  0x6e04]**; ignored unless the slot is allocated and code < 13:

  | code | value | effect at the next tick |
  |---|---|---|
  | 0 | u8 | stop: 1 = key off all notes (release), else cut all notes; marks the slot "stop requested" |
  | 1 | u8 | 0 clears "paused"; 1 sets it and keys notes off; else sets it and cuts notes (the sequencer itself does not pause) |
  | 2 | u8 | effect send switch for the slot's notes [I] |
  | 3 | h8 L, h10 R | volume scale, 128 = unity |
  | 4 | h8 | pitch scale, 4096 = 1.0 |
  | 5 | h8 | stored, never read |
  | 6-10 | u8 | single-note slots only: channel volume, pan, bend, LFO depth, LFO speed |
  | 11 | u8 | voice group override 0..2 |
  | 12 | h8 | exclusive-owner override |

  EE: stop = `{id, 0, release ? 1 : 2}` (EE 0x23e020); every play is
  followed by code 3 with `(global * master * vol) >> 14`.
* **131** status **[V IRX 0x8800]**: for the i-th listed slot (oldest
  first) `out[4i] = slot`, `out[4i+1] = flags & 0xFF`; bytes 2-3 untouched;
  `out[256]` u32 = count. Flag bits: 1 allocated, 2 active, 8 paused.

  | state | byte |
  |---|---|
  | free | not listed |
  | requested / playing | 0x03 |
  | sequence ended and all notes ended | 0x01 (stays listed until stopped) |
  | paused | 0x0B, 0x09 once the notes are gone |

### 9.2 Slot life cycle **[V IRX 0x8878 tick: 0x6e04, 0x8988, 0x7260, 0x8020, 0x81b4]**

1. Start request: look up the SE in SEIF -> sequence pointer, tempo, voice
   group (single-note slots start their note here and are "ended" at once).
   An unregistered SE leaves a null sequence pointer which the real driver
   then interprets (a bug); it prints `unregisted SE request`.
2. Sequencer (9.3) starts notes; the sequence's end code sets "ended".
3. Each tick, notes whose voices have all ended are freed; when the slot is
   "ended" and has no notes left, flag bit 2 (active) is cleared. That is
   what the game waits for: EE 0x23e8a0 copies bit 2 and sets its own
   "finished" bit when it is clear; the game (0x181614) polls
   `state(id) & 4` (EE 0x23e150) each frame and then calls stop(id, release = 1).
   With keep = 0 the EE library stops the slot itself (cut).
4. The driver frees and unlists a slot **only** after a code-0 stop, once
   its note list is empty (same tick for a cut; after the voices have died
   for a release stop). The EE returns the id to its free list when the
   slot is no longer listed.

There are no timeouts anywhere: an SE made of a looping sample stays active
until it is stopped.

Note end = all its voices released by the voice manager's ENVX rule
(section 5): sample end block reached (or key off + ADSR release), plus
9 ticks.

### 9.3 SE sequence byte-code **[V IRX 0x7260, table 0xdb38]**

Timer: a 20.12 fixed-point counter starting at 0; each tick it is decreased
by `tempo * 4096 / rate` (rate = config code 1, 60) and opcodes run while it
is negative. Every opcode except 0 is followed by a variable-length delta
(7 bits per byte, most significant first, 0x80 = continue); `timer += delta
<< 12`. Opcode byte: low 6 bits = opcode, 0x40 = "broadcast" flag.

| op | operands | meaning |
|---|---|---|
| 0 | - | end of sequence (no delta) |
| 1, 11 | - | no operation (wait) |
| 2 | u8 n | loop start, body runs n+1 times, 0 = forever |
| 3 | - | loop end |
| 4 | vl note id, vl channel, u8 program, u8 key, u8 velocity | note on |
| 5 | vl note id | key off the first note with that id |
| 6 | vl note id | cut that note |
| 7 / 9 | vl channel, u8 param, u8 value | set / add channel parameter (0 volume, 1 pan, 2 bend, 3 LFO depth, 4 LFO speed, 5 transpose) |
| 8 / 10 | vl channel, u8 param, u8 target, vl duration | fade channel parameter |
| >= 12 | | `unknow SE sequence code`, the tick is abandoned |

With the broadcast flag, ops 5-10 act on every active slot that has a note
whose voice-layer handle equals the operand or a channel whose id equals the
operand (IRX 0x74f4-0x7650; the handle comparison looks like a driver bug),
instead of the own slot only.

On disc (1111 SEs in 34 banks): 1097 are `[note on, end]`, 14 are
`[broadcast key off, note on, end]`; all deltas are 0; no loops, fades or
parameter changes.

Note on (IRX 0x8a5c -> 0x8f10): every tone of the program whose key range
contains the key gets a voice (`0x3b80(-1, tone[1], auto, group, owner,
0x9e50, entry)`), `SSA = bank SPU base + tone[20]`, ADSR from the tone,
pitch = `sceSdNote2Pitch(root, fine, key, 0) * ((slot pitch * tone[12]) >> 12) >> 12`,
then key on. If any tone cannot get a voice the whole note is dropped.

---------------------------------------------------------------------------

## 10. ADPCM streams (160-164)

Used for voices/music read from `/DATA/SND/XWS/*.STR`. The EE reads the
file and feeds raw SPU ADPCM; the `.STR` container itself is parsed by game
code outside the sound library and is **not** covered here **[?]**. The
game opens streams through EE 0x23fbf0 (caller 0x180a28: rate 48000, 4 KiB
IOP ring per channel).

### 10.1 Data path

```
EE --sceSifSetDma--> IOP ring (per channel) --type-1 transfer--> SPU ring (per channel) --looping voice-->
        cmd 161 "n bytes added"            driver tick, <= 1024 bytes per pass
```

EE write function 0x240180 **[V]**: per chunk (<= 16384 bytes) it copies the
blocks to a staging buffer, **clears the flag byte of every 16-byte block**,
then sets flag **3** (loop end + repeat) on the block that will land at the
end of the SPU ring (it tracks the SPU ring write position itself, slot+52
modulo the SPU ring size), writes the chunk to `iop_buf + write_pos` with
`iop_write`, and queues command 161 with the byte count rounded down to a
multiple of 64 (the remainder is carried). Stereo modes de-interleave into
the per-channel rings (channel k's IOP ring is `iop_buf + k * iop_size`).
End of stream, EE 0x240a50: pad to 64 bytes, write one extra 64-byte block
whose first line has flag **1** (end, no repeat; data at EE 0x2f1680), and
queue command 161 with end flag 1.

### 10.2 Commands

* **160** open **[V IRX 0x9eb0; EE 0x23fc30]**:

  ```
  b0  slot (0..47, chosen by the EE)       b2  channel count
  h4  voice priority (-1 from EE entry 0x23fbf0)
  h6  pitch = rate * 4096 / 48000
  w8  IOP ring size per channel            w12 SPU ring size per channel
  +16 { u32 iop_buf; u32 spu_buf; } per channel
  ```

  Result 1, or 0 if the slot is already open (`failed to open ADPCM slot`).
  The slot is linked at the **head** of the open list immediately; volumes
  128/128, pitch scale 4096. EE channel counts: mode 0 -> 1, modes 1, 2 -> 2,
  mode `0x1000000 | n` -> n. If the EE allocates the buffers itself it uses
  command 32 (IOP) and its SPU heap.
* **161** `{b0 slot, b1 end flag, w4 bytes}` **[V IRX 0xad94]**: the bytes
  (clamped to the free space) are added to the IOP ring fill count and write
  position; `end flag` is stored (1 = no more data will follow).
* **162** `{w0 slot, w4 code, +8 value}` **[V IRX 0xaea0, table 0xdcc0;
  applied at 0xa140]**:

  | code | value | effect at the next tick |
  |---|---|---|
  | 0 | - | close: voices stopped, pending transfers cancelled, slot unlisted |
  | 1 | u8 | pause on (voices stopped, position kept) / off |
  | 2 | u8 | effect send: voice mix mode 3 (dry + effect) or 1 (dry) |
  | 3 | h8 L, h10 R | stream volume (128 = unity) |
  | 4 | h8 | pitch scale, 4096 = 1.0 |
  | 5 | | no effect |
  | 6 | h8 (0..2) | voice group for the first channel's voice |
  | 7 | h8 (0..127) | exclusive-owner id (`value \| 0x2000000`) |

* **164** `{h0 slot, h2 channel, w4 code, +8 value}` **[V IRX 0xb01c]**:
  code 3 = channel volume `{h8 L, h10 R}`, code 5 = preferred voice number
  (w8) for that channel.
* **163** status **[V IRX 0xb160]**, out 772 bytes: for the i-th open slot
  (newest first) a 16-byte record

  ```
  b0  slot            b1  flags & 0xFF: 1 open, 2 playing, 4 finished, 8 paused
  w4  free bytes in the IOP ring = iop_size - (queued + in transfer)
  w8  bytes played, high word        w12 bytes played, low word
  ```

  `out[768]` u32 = number of records. When the stream has finished, both
  position words are 0xFFFFFFFF.

  EE consumer 0x241610 **[V]**: copies flag bits 2/4/8, sets its fill count
  to `iop_size - w4` (that is how "need more data" is derived: the write
  function sends at most the free space), stores position `w8 | w12`, fires
  position callbacks registered with EE 0x240c38 when the position passes
  them, and releases its slot (and frees the buffers it owns) once a closed
  slot is **no longer listed**.

### 10.3 Driver state machine (tick IRX 0xb1dc: 0xa140, 0xb250, 0xa3ec, 0xa9e0) **[V]**

* `threshold` = 1.5 ticks worth of data: `roundup64(((pitch * scale) >> 12)
  * (27428 / rate) * 3 / 8192)` (704 bytes at 48 kHz).
* Not playing, not paused: if the end flag is clear and the SPU ring holds
  less than `threshold`, stay stopped. Otherwise allocate one voice per
  channel (`0x3b80(preferred, priority, no auto, group, owner, 0xb3c4,
  channel)`), set `SSA = spu_buf + play_pos`, `LSAX = spu_buf` (omitted when
  the end flag is set and the remaining data does not wrap), pitch =
  `Note2Pitch(64,0,64,0) * ((pitch * scale) >> 12) >> 12`, key on; flag
  "playing".
* Playing: `pos = (NAX(first channel's voice) - spu_buf) & ~63`
  (`sceSdGetAddr 0x2240`, IRX 0x4468). If the end flag is clear and the SPU
  ring holds less than `threshold`: underrun, voices stopped, "playing"
  cleared (it restarts by itself when data arrives). Otherwise the play
  position, the SPU ring fill and the 64-bit played counter advance by the
  distance moved.
* End flag set and a channel voice's ENVX reads 0 (the voice ran into the
  flag-1 block): voices stopped, position = -1, flag "finished". Nothing
  more happens until the EE closes the slot.
* A voice stolen by another owner: all channel voices stopped; "finished"
  is set if the priority is >= 0.
* Feeding: per tick, up to the free space of the SPU ring, in pieces of at
  most 1024 bytes limited by both ring wraps, queue-0 type-1 transfers copy
  IOP ring -> SPU ring for every channel.

---------------------------------------------------------------------------

## 11. PCM streams (176-179) — unused by the game

**[V IRX 0xb4a0-0xce60; EE 0x23ebd0-0x23f830]**

A software mixer on the IOP: at init `sceSdBlockTrans(1, SD_TRANS_WRITE |
LOOP (0x10), 0x36858, 8192)` plays an 8192-byte buffer in a loop through
core 1's block input (BVOLL/BVOLR = 0x7FFF). Each half (4096 bytes = 1024
stereo samples at 48 kHz, 21.3 ms) is refilled while the SPU plays the
other half (`sceSdBlockTransStatus`). Every refill consumes **one block per
stream**.

* **176** `{b0 slot (0..31, EE-chosen), b1 mode, w4 IOP buffer, w8 block
  count}` -> 1, or 0 if the slot is open. Modes: 0 mono 16-bit, 2048-byte
  blocks; 1 stereo as two planes (left blocks, then right blocks at
  `buffer + count * 2048`), 2048-byte blocks; 2 and 3 interleaved stereo,
  4096-byte blocks (3 XORs each sample with a mask). The buffer is a ring of
  `count` blocks.
* **177** `{b0 slot, b1 n, +4 u32 tag[n]}`: n more blocks were written; each
  carries a tag (the EE uses a running block counter). Fill count += n
  (clamped to the ring size).
* **178** `{w0 slot, w4 code, +8 value}`: 0 close; 1 pause (0 run, 1 or 2
  paused); 2 XOR masks (two u16); 4 volume `{h8 L, h10 R}` (-128..128
  through the gain table at 0xe2e0); 5 "no more data": the stream is
  finished when the playing tag reaches the last tag supplied; 3 and >= 6
  no effect.
* **179** status, out 388 bytes: 12-byte records for open slots (newest
  first) `{b0 slot, b1 flags (1 open, 2 playing, 4 finished, 8 paused),
  h2 blocks still queued, w4 tag of the block now playing, w8 sample offset
  inside it}`; `out[384]` = count. The EE derives free space as
  `(count - queued) * block_size` and fires position callbacks from the tag
  and offset.

---------------------------------------------------------------------------

## 12. The implementation (`recomp/rt/iop_rxsnd.c`, `recomp/rt/audio_out.c`)

* The worker thread is `driver_tick`, run lazily: each RPC (and each
  `rxsnd_mix` call) first runs the ticks due in virtual time (20 ms each), so
  requests take effect "at the next vblank" exactly as in the driver.
* Under the driver sits an SPU2 voice mixer: 2 MiB of sound RAM filled by the
  same transfers as on hardware and 48 voices with ADPCM decoding (5 filters,
  loop start / end / repeat flags), 12-bit pitch counter with 4-point
  interpolation, the SPU2 ADSR (ADSR1: attack mode/shift/step, decay shift,
  sustain level; ADSR2: sustain mode/direction/shift/step, release
  mode/shift), per-voice L/R volume with the driver's fade scale, and master
  volume per core. Each driver tick renders 960 stereo frames into a FIFO;
  `rxsnd_mix(out, frames)` drains it. NAX and ENVX read by the driver logic
  are the mixer's real values, so SE completion and stream position/finish
  come from the same logic as on the console.
* SE note volume follows IRX 0x9508-0x96f4: `note volume * tone volume *
  VELT[velocity] >> 7 * PANT[pan] >> 7`, with note volume = SE definition
  volume * control-code-3 scale; pan is fixed at centre.
* Not implemented in the mixer: reverb (the game does select a type and
  depth with command 50; voices are heard dry), noise, pitch modulation,
  LFOs, pitch bend, the PCM stream mixer (section 11, unused), SPDIF modes.
  The interpolation is Catmull-Rom, not the SPU2's Gaussian table.
* Other deviations, invisible to the EE: transfers complete inside command
  64 (65 always returns 0); XVAB headers are not relocated in place; SE
  control codes are applied at once; an unregistered SE ends at once; the
  IOP heap is a private 512 KiB arena.
* Stops with a message instead of guessing: command 112 (BGM), XVAB/XSEQ
  version >= 0x200, SE opcodes 2, 3, 7-10 and >= 12, out-of-range slot ids,
  malformed argument blocks.
* `audio_out.c`: `audio_frame()` once per video frame pulls 960 frames and
  writes them to `$ROR_WAV` and, in real-time runs, to PulseAudio by speaking
  its native protocol over the unix socket (no libpulse on the machine).
* Debug: `ROR_SNDDBG` (stream status trace), `ROR_SNDDUMP=prefix` (raw ADPCM
  as fed to each stream), `ROR_AUDIODBG` (device statistics at exit).
* Checks: `recomp/tools/rxsnd_test.sh` (protocol, with the real `0000.SE`),
  `recomp/tools/wav_check.py` (levels, DC, clipping, frame-boundary
  discontinuities, spectrum, spectrogram PNG), `recomp/tools/stream_compare.py`
  (mixer output against an independent offline decode of a fed stream).
* `/DATA/SND/XWS/*.STR` as observed: headerless SPU ADPCM, stereo
  interleaved one 16-byte block per channel, 48 kHz; `s100000.str` (title
  BGM) is at disc sector 725251 and every block fed to the driver matched the
  disc bytes **[V for that file]**.

## 13. Open questions

1. `.STR` files other than the stereo title BGM (mono/other rates, how the
   game's file index maps names to sectors) were not examined.
2. Reverb is not implemented; SE loudness and pan were not compared with
   real hardware.
3. Whether ENVX can read 0 during the attack of a real voice; the driver's
   9-tick rule suggests the authors saw transient zeros. The model never
   reports 0 for a sounding voice.
4. XVAB/XSEQ version >= 0x200 chunk headers (two unknown words).
5. Meaning of config codes 3-5 and of `sceSdSetCoreAttr` entry 10 values.
6. Tone bytes 24-32 (LFO) and the VELT/PANT curves are described from the
   arithmetic only.
7. BGM details (section 8) were read once and never exercised.
