"""games.py — the editions of the game this project knows, and what is specific to each.

One entry per disc. Everything that depends on a particular build of the game's
executable is listed here and nowhere else: gen.py writes it into the generated
funcs.h (as ROR_GAME_* / ROR_FN_* macros), rt/support.c turns those into the
`ror_game` structure the runtime reads, tools/vu1extract.py and
release/ror_build.py look their entries up here.

  key            short name used by the build scripts (GAME=pal|ntsc) and in directory names
  serial         as printed on the disc
  elf            name of the executable in the root directory of the disc
  sha256         of that executable
  region, video  for messages; video also selects the timing of the runtime (PAL: 50 Hz, 312 lines;
                 NTSC: 59.94 Hz, 262.5 lines)
  fn_camera      the function that builds a camera (rt/wide.c hooks it)
  guard_band     address of the two floats holding the guard band's x limits (rt/wide.c)
  fn_vu1_upload  the function that sends the VU1 micro program (tools/vu1extract.py)
"""
import hashlib

GAMES = [
    dict(key='pal', serial='SLES-54218', elf='SLES_542.18', region='Europe', video='PAL',
         sha256='71417965b8931cadcebc33f48439df3290822316e4c139d9c4c7a4b6d7fb7692',
         fn_camera=0x228b80, guard_band=0x32a5e4, fn_vu1_upload=0x221000),
    dict(key='ntsc', serial='SLUS-21448', elf='SLUS_214.48', region='USA', video='NTSC',
         sha256='262efe713ad2432d4e8b2122a5edae9302b7f9eb900537b76beae064a4a91af3',
         fn_camera=0x226c58, guard_band=0x3035d4, fn_vu1_upload=0x21f0d8),
]


def by_key(key):
    for g in GAMES:
        if g['key'] == key:
            return g
    return None


def by_elf_name(name):
    for g in GAMES:
        if g['elf'].upper() == name.upper():
            return g
    return None


def by_elf_data(data):
    """The entry whose executable this is, or None (another edition or revision)."""
    h = hashlib.sha256(data).hexdigest()
    for g in GAMES:
        if g['sha256'] == h:
            return g
    return None


if __name__ == '__main__':            # games.py KEY FIELD: for the shell scripts
    import sys
    g = by_key(sys.argv[1])
    if g is None:
        raise SystemExit('games.py: unknown game %s (known: %s)' % (sys.argv[1], ', '.join(x['key'] for x in GAMES)))
    v = g[sys.argv[2]]
    print('0x%x' % v if isinstance(v, int) else v)
