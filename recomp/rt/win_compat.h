/* win_compat.h — force-included (-include) in every Windows compile by
 * recomp/build-win.sh: the few POSIX names used by runtime files that are
 * otherwise portable and that mingw-w64 does not provide. */
#ifndef ROR_WIN_COMPAT_H
#define ROR_WIN_COMPAT_H
#ifdef _WIN32
#define _SC_NPROCESSORS_ONLN 84
long ror_win_sysconf(int name);
#define sysconf(name) ror_win_sysconf(name)
#endif
#endif
