/* pad_ps.h — Sony DualShock 4 / DualSense HID reports (pad_ps.c). */
#ifndef ROR_PAD_PS_H
#define ROR_PAD_PS_H
#include "plat.h"

enum { PS_DS4 = 1, PS_DUALSENSE = 2 };
enum { PS_EXTRA_PS = 1, PS_EXTRA_TOUCHPAD = 2 };    /* buttons with no PS2 counterpart */

/* Decode one input report (d[0] = report ID, n bytes) of a pad connected by
 * USB (bt = 0) or Bluetooth (bt = 1). Returns 1 and fills *out (and *extra,
 * if not NULL) when the report carries the pad state, 0 otherwise. */
int ps_parse(int model, int bt, const u8 *d, int n, HostPad *out, int *extra);
/* Build the output report that sets the two motors (0..255 each) and nothing
 * else. out must hold 78 bytes; returns the report length. */
int ps_rumble_report(int model, int bt, int small, int big, u8 *out);
u32 ps_crc32(u32 crc, const u8 *p, size_t n);       /* CRC-32 (IEEE), crc = 0 to start */
int ps_selftest(void);                              /* ROR_PAD_SELFTEST=1: decode sample reports, print, 0 = all as expected */
#endif
