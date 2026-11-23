/* video_x11.c — the game window on X11 (plat.h vwin_*). */
#ifndef _WIN32
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include "rt.h"
#include "plat.h"

static Display *dpy; static Window win; static GC gc; static XImage *img;
static int img_w, img_h, cur_w, cur_h, is_full;
static Atom wm_delete, net_state, net_full;

static void set_fullscreen(int on) {             /* EWMH: ask the window manager */
    XEvent ev; memset(&ev, 0, sizeof ev);
    ev.xclient.type = ClientMessage; ev.xclient.window = win; ev.xclient.message_type = net_state; ev.xclient.format = 32;
    ev.xclient.data.l[0] = on ? 1 : 0; ev.xclient.data.l[1] = (long)net_full; ev.xclient.data.l[3] = 1;
    XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureNotifyMask | SubstructureRedirectMask, &ev);
    is_full = on;
}
int vwin_open(int w, int h, int fullscreen) {
    dpy = XOpenDisplay(0);
    if (!dpy) return 0;
    int scr = DefaultScreen(dpy);
    video_fit(&w, &h, DisplayWidth(dpy, scr), DisplayHeight(dpy, scr));
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 80, 80, (unsigned)w, (unsigned)h, 0, 0, 0);
    XStoreName(dpy, win, "Rule of Rose");
    XSelectInput(dpy, win, KeyPressMask | KeyReleaseMask | StructureNotifyMask);
    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    net_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    net_full = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    gc = XCreateGC(dpy, win, 0, 0);
    cur_w = w; cur_h = h;
    if (fullscreen) {                            /* initial state: read by the window manager when the window is mapped */
        XChangeProperty(dpy, win, net_state, 4 /* XA_ATOM */, 32, PropModeReplace, (unsigned char *)&net_full, 1);
        is_full = 1;
    }
    XMapWindow(dpy, win);
    return 1;
}
static u16 key_bit(KeySym k) {
    switch (k) {
    case XK_Up: return 0x0010; case XK_Right: return 0x0020; case XK_Down: return 0x0040; case XK_Left: return 0x0080;
    case XK_z: return 0x4000; case XK_x: return 0x2000; case XK_a: return 0x8000; case XK_s: return 0x1000;   /* cross circle square triangle */
    case XK_Return: return 0x0008; case XK_BackSpace: case XK_Shift_R: return 0x0001;                          /* start select */
    case XK_q: return 0x0400; case XK_w: return 0x0800; case XK_e: return 0x0100; case XK_r: return 0x0200;   /* L1 R1 L2 R2 */
    }
    return 0;
}
void vwin_pump(u16 *pad_buttons) {
    while (XPending(dpy)) {
        XEvent ev; XNextEvent(dpy, &ev);
        if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == wm_delete) exit(0);
        if (ev.type == ConfigureNotify) { cur_w = ev.xconfigure.width; cur_h = ev.xconfigure.height; }
        if (ev.type == KeyPress || ev.type == KeyRelease) {
            KeySym k = XLookupKeysym(&ev.xkey, 0);
            if (k == XK_Escape && ev.type == KeyPress) exit(0);
            if (ev.type == KeyPress && (k == XK_F11 || (k == XK_Return && (ev.xkey.state & Mod1Mask)))) { set_fullscreen(!is_full); continue; }
            u16 bit = key_bit(k);
            if (ev.type == KeyPress) *pad_buttons &= (u16)~bit; else *pad_buttons |= bit;
        }
    }
}
void vwin_size(int *w, int *h) { *w = cur_w; *h = cur_h; }
void vwin_show(const u32 *px, int w, int h) {
    if (!img || img_w != w || img_h != h) {
        if (img) { img->data = 0; XDestroyImage(img); }
        img = XCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), 24, ZPixmap, 0, 0, (unsigned)w, (unsigned)h, 32, 0);
        img_w = w; img_h = h;
    }
    img->data = (char *)px;
    XPutImage(dpy, win, gc, img, 0, 0, 0, 0, (unsigned)w, (unsigned)h);
    img->data = 0;
}
void vwin_flush(void) { XFlush(dpy); }
void vwin_native(void **a, void **b) { *a = dpy; *b = (void *)win; }
/* Refresh rate from XRandR, loaded at run time (the build links only libX11). */
#include <dlfcn.h>
int vwin_refresh_hz(void) {
    static int hz = -1;
    if (hz >= 0) return hz;
    hz = 60;
    void *l = dlopen("libXrandr.so.2", RTLD_NOW);
    if (!l || !dpy) return hz;
    void *(*get_info)(Display *, Window) = (void *(*)(Display *, Window))dlsym(l, "XRRGetScreenInfo");
    short (*cur_rate)(void *) = (short (*)(void *))dlsym(l, "XRRConfigCurrentRate");
    void (*free_info)(void *) = (void (*)(void *))dlsym(l, "XRRFreeScreenConfigInfo");
    if (get_info && cur_rate && free_info) {
        void *cfg = get_info(dpy, DefaultRootWindow(dpy));
        if (cfg) { int r = cur_rate(cfg); if (r >= 24 && r <= 1000) hz = r; free_info(cfg); }
    }
    return hz;
}
#endif
