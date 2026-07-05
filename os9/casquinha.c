/*
 * casquinha.c — the Mac OS 9 app (Fio 3).
 *
 * A classic PowerPC Toolbox application: a document window, a menu bar
 * (Apple / File ▸ Quit), and a WaitNextEvent loop that also drives the LIVE
 * gopher wire. Every ~2 s (a TickCount delta — the loop is the only clock) it
 * starts a /spot/api/1/now transaction over Open Transport (cq_transport_ot),
 * advances it non-blocking each loop pass, adopts the reply through the
 * monotonic ts-guard (cq_guard), and renders the snapshot in Monaco. On a
 * network failure it shows "offline - retrying" and keeps the last snapshot.
 *
 * The pure core (cq_codec/cq_now/cq_guard) and the transport are the SAME code
 * verified on the host; only this glue and the OT backend are OS 9 specific.
 * Built with Retro68's classic-PPC toolchain + Apple Universal Interfaces (OT).
 */
#include <Quickdraw.h>
#include <Windows.h>
#include <Controls.h>
#include <ControlDefinitions.h>
#include <Menus.h>
#include <Fonts.h>
#include <Events.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <ToolUtils.h>
#include <Devices.h>
#include <Timer.h>
#include <Appearance.h>
#include <TextCommon.h>
#include <TextEncodingConverter.h>
#include <Movies.h>
#include <ImageCompression.h>
#include <QDOffscreen.h>
#include <Components.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "cq_codec.h"
#include "cq_now.h"
#include "cq_guard.h"
#include "cq_backoff.h"
#include "cq_debounce.h"
#include "cq_transport.h"

enum {
    kMenuBar    = 128,
    kAppleMenu  = 128,
    kFileMenu   = 129,
    kAboutAlert = 128,
    kMainWindow = 128,
    kAboutItem  = 1,
    kQuitItem   = 1
};

#define CQ_HOST       "10.0.100.112"
#define CQ_PORT       70
#define CQ_POLL_TICKS 120           /* 2 s at 60 ticks/sec (law 5: >= micro-cache) */
#define CQ_POLL_CAP   1800          /* back off to at most 30 s on errors (429) */

/* HiWord/LoWord aren't in the interfaces uniformly; compute them. */
#define CQ_HIWORD(x) ((short)((x) >> 16))
#define CQ_LOWORD(x) ((short)((x) & 0xFFFF))

static Boolean       gRunning = true;
static WindowRef     gWindow  = NULL;
static short         gDepth   = 1;       /* main-device depth, for theme colors */
static Boolean       gIsColor = false;
static unsigned long gSnapTick = 0;      /* TickCount when gSnap was adopted */

static cq_guard      gGuard;
static cq_backoff    gBackoff;           /* poll cadence, backs off on errors */
static cq_now        gSnap;              /* current adopted snapshot */
static Boolean       gHaveSnap = false;
static Boolean       gOnline   = false;
static cq_transport *gTx       = NULL;   /* in-flight poll, or NULL */
static unsigned long gLastPoll = 0;

/* --- runtime diagnostics (this is the UTM debugging pass) --- */
static long          gPolls     = 0;     /* transactions started */
static long          gDone      = 0;     /* transactions completed */
static int           gLastStat  = -1;    /* last cq_tx_status seen */
static int           gLastErr   = 0;     /* last cq_tx_error code */
static long          gLastLen   = 0;     /* bytes of last reply */
static char          gLastMsg[128] = "";
static unsigned long gLastDraw  = 0;

/* --- transport controls (Fio 4) --- */
static ControlHandle gPrev = NULL, gPlay = NULL, gNext = NULL, gVol = NULL;
static cq_debounce   gDeb;               /* pre-wire coalescer for prev/next (law 1) */
static unsigned long gCmdFire = 0;       /* tick to flush the debounced command */
static cq_transport *gCmd = NULL;        /* in-flight command transaction */
static unsigned long gVolHold = 0;       /* ignore poll volume until this tick (law 4) */

#define CQ_DEBOUNCE_TICKS 18             /* ~0.3 s settle before a prev/next reaches the wire */
#define CQ_HOLD_TICKS    180             /* ~3 s single hold window on the volume slider */

/* --- cover art (Fio 5) --- */
#define CQ_COVER_PX 64
static Boolean       gQTOk    = false;   /* QuickTime available (EnterMovies ok) */
static cq_transport *gCover   = NULL;    /* in-flight cover fetch */
static GWorldPtr     gCoverGW = NULL;    /* decoded 64x64 cover */
static char          gCoverAlbum[64] = "";  /* album_id currently decoded (or tried) */
static char          gCoverReq[64]   = "";  /* album_id being fetched */

/* -------------------------------------------------------------------------- */

/* The wire is UTF-8; QuickDraw draws MacRoman. Convert at the draw boundary only
 * (NOTES.md), via the Text Encoding Converter. ASCII passes through unchanged, so
 * headers/times/numbers are unaffected; accents (ç ã é ê õ) render correctly. */
static TECObjectRef gConv = NULL;

static short ToMacRoman(const char *s, char *dst, short dstmax)
{
    ByteCount srcLen = (ByteCount)strlen(s), srcRead = 0, dstLen = 0;
    if (gConv) {
        OSStatus e = TECConvertText(gConv, (ConstTextPtr)s, srcLen, &srcRead,
                                    (TextPtr)dst, (ByteCount)dstmax, &dstLen);
        if (e == noErr) return (short)dstLen;
    }
    { short n = (short)(srcLen < (ByteCount)dstmax ? srcLen : dstmax);  /* ASCII fallback */
      memcpy(dst, s, n); return n; }
}

static void DrawCStr(short x, short y, const char *s)
{
    char buf[256];
    short n = ToMacRoman(s, buf, sizeof(buf));
    MoveTo(x, y);
    DrawText(buf, 0, n);
}

/* Apply a theme text color at the current device depth. */
static void ThemeText(ThemeTextColor c) { SetThemeTextColor(c, gDepth, gIsColor); }

/* Right-align a C string ending at x=right, baseline y (measure the MacRoman). */
static void DrawCStrRight(short right, short y, const char *s)
{
    char buf[256];
    short n = ToMacRoman(s, buf, sizeof(buf));
    short w = TextWidth(buf, 0, n);
    MoveTo(right - w, y);
    DrawText(buf, 0, n);
}

static void FmtTime(char *out, long ms)
{
    long s = ms / 1000;
    snprintf(out, 16, "%ld:%02ld", s / 60, s % 60);
}

static void DrawWindowContents(WindowRef win)
{
    Rect  pr = ((GrafPtr)win)->portRect;
    short W  = pr.right - pr.left;
    char  line[128];
    Rect  sep;

    SetPort((GrafPtr)win);
    /* Platinum: fill with the theme window background, not raw white. */
    SetThemeWindowBackground(win, kThemeBrushDialogBackgroundActive, false);
    EraseRect(&pr);
    DrawControls(win);        /* transport buttons + volume slider (Fio 4) */

    /* Header: app name + a green gopher-spot accent rule. */
    TextFont(0); TextFace(bold); TextSize(11);   /* Charcoal, the system font */
    ThemeText(kThemeTextColorDialogActive);
    DrawCStr(16, 22, "Casquinha");
    TextFace(0);
    if (gIsColor) {
        RGBColor green = { 7453, 47545, 21588 };   /* gopher-spot / Spotify green */
        RGBColor save;
        GetForeColor(&save);
        RGBForeColor(&green);
        SetRect(&sep, 16, 28, W - 16, 30);
        PaintRect(&sep);
        RGBForeColor(&save);
    } else {
        SetRect(&sep, 16, 28, W - 16, 29);
        DrawThemeSeparator(&sep, kThemeStateActive);
    }

    if (!gHaveSnap) {
        TextFont(1); TextSize(10);               /* Geneva */
        ThemeText(kThemeTextColorDialogActive);
        DrawCStr(16, 64, gLastMsg[0] ? gLastMsg : "Connecting...");
        return;
    }

    /* Track title — bold system font; green when actually playing. */
    TextFont(0); TextFace(bold); TextSize(14);
    if (gSnap.state == CQ_STATE_PLAYING && gIsColor) {
        RGBColor green = { 7453, 47545, 21588 }, save;
        GetForeColor(&save); RGBForeColor(&green);
        DrawCStr(16, 58, gSnap.track ? gSnap.track : "(no track)");
        RGBForeColor(&save);
    } else {
        ThemeText(kThemeTextColorDialogActive);
        DrawCStr(16, 58, gSnap.track ? gSnap.track : "(no track)");
    }
    TextFace(0);

    /* Artist / album — Geneva, dimmed. */
    TextFont(1); TextSize(10);
    ThemeText(kThemeTextColorDialogInactive);
    DrawCStr(16, 78, gSnap.artist ? gSnap.artist : "");
    DrawCStr(16, 94, gSnap.album  ? gSnap.album  : "");

    /* Progress bar — native themed track, interpolated between polls. */
    {
        long posMs = (long)gSnap.position_ms;
        long durMs = (long)gSnap.duration_ms;
        if (gSnap.state == CQ_STATE_PLAYING) {
            long elapsed = (long)(((TickCount() - gSnapTick) * 1000L) / 60L);
            posMs += elapsed;
        }
        if (posMs < 0) posMs = 0;
        if (durMs > 0 && posMs > durMs) posMs = durMs;

        if (durMs > 0) {
            ThemeTrackDrawInfo info;
            SetRect(&info.bounds, 16, 108, W - 16, 124);
            info.kind        = kThemeProgressBar;
            info.min         = 0;
            info.max         = durMs;
            info.value       = posMs;
            info.reserved    = 0;
            info.attributes  = kThemeTrackHorizontal;
            info.enableState = kThemeTrackActive;
            info.filler1     = 0;
            info.trackInfo.progress.phase = 0;
            DrawThemeTrack(&info, NULL, NULL, 0);
        }
        TextFont(1); TextSize(9);
        ThemeText(kThemeTextColorDialogInactive);
        { char t[16]; FmtTime(t, posMs); DrawCStr(16, 140, t); }
        { char t[16]; FmtTime(t, durMs); DrawCStrRight(W - 16, 140, t); }
    }

    /* "Vol" label to the left of the volume slider. */
    TextFont(1); TextSize(9);
    ThemeText(kThemeTextColorDialogInactive);
    DrawCStr(276, 173, "Vol");

    /* State / device row (below the transport buttons). */
    TextFont(1); TextSize(10);
    ThemeText(kThemeTextColorDialogActive);
    snprintf(line, sizeof(line), "%s      %s",
             gSnap.state == CQ_STATE_PLAYING ? "Playing" :
             gSnap.state == CQ_STATE_PAUSED  ? "Paused"  : "Stopped",
             gSnap.device == CQ_DEV_ACTIVE ? "on gopher-spot" :
             gSnap.device == CQ_DEV_IDLE   ? "playing elsewhere" : "");
    DrawCStr(16, 202, line);

    /* Album cover (Fio 5), top-right, drawn last so it sits above the text. */
    if (gCoverGW) {
        PixMapHandle pm = GetGWorldPixMap(gCoverGW);
        Rect sr, dr;
        SetRect(&sr, 0, 0, CQ_COVER_PX, CQ_COVER_PX);
        SetRect(&dr, W - 16 - CQ_COVER_PX, 44, W - 16, 44 + CQ_COVER_PX);
        LockPixels(pm);
        CopyBits((BitMap *)*pm, &((GrafPtr)win)->portBits, &sr, &dr, srcCopy, NULL);
        UnlockPixels(pm);
    }

    /* A quiet status line only when something's wrong. */
    if (gLastMsg[0]) {
        TextFont(1); TextSize(9);
        ThemeText(kThemeTextColorDialogInactive);
        DrawCStr(16, 220, gLastMsg);
    }
}

static void Redraw(void)
{
    if (gWindow) DrawWindowContents(gWindow);
}

/* Keep the Play/Pause button title in sync with the current state. */
static void UpdatePlayTitle(void)
{
    if (gPlay)
        SetControlTitle(gPlay, gSnap.state == CQ_STATE_PLAYING ? "\pPause" : "\pPlay");
}

/*
 * Adopt a /now-shaped reply from EITHER a poll or a command. Checks the error
 * key first (law 6), adopts a healthy snapshot through the ts-guard (laws 2/3),
 * and drives the backoff. A command's reply is authoritative — it comes through
 * the same guard, so no catch-up poll storm (law 3).
 */
static void AdoptReply(const unsigned char *d, size_t len)
{
    cq_fields f;
    const char *err;

    gLastLen = (long)len;
    gDone++;
    gOnline = true;

    cq_fields_init(&f);
    cq_fields_parse(&f, d, len);
    err = cq_fields_get(&f, "error");
    if (err) {
        if (strcmp(err, "upstream") == 0)
            snprintf(gLastMsg, sizeof(gLastMsg), "Spotify busy (rate limited) - easing off");
        else
            snprintf(gLastMsg, sizeof(gLastMsg), "server error: %s", err);
        cq_backoff_fail(&gBackoff);
    } else {
        cq_now tmp;
        cq_backoff_ok(&gBackoff);
        cq_now_from_fields(&tmp, &f);
        if (cq_guard_accept_ts(&gGuard, tmp.ts)) {
            int volHeld = (long)((TickCount() - gVolHold)) < 0;  /* within hold window? */
            int keepVol = gSnap.volume;
            cq_now_free(&gSnap);
            gSnap = tmp;
            gHaveSnap = true;
            gSnapTick = (unsigned long)TickCount();
            gLastMsg[0] = '\0';
            if (volHeld && gHaveSnap) gSnap.volume = keepVol;    /* don't yank a live drag */
            UpdatePlayTitle();
            if (gVol && !volHeld && gSnap.volume >= 0)
                SetControlValue(gVol, gSnap.volume);
        } else {
            cq_now_free(&tmp);
        }
    }
    cq_fields_free(&f);
    Redraw();
}

/* Advance one gopher transaction (poll or command). Returns 1 when it finished
 * (and was freed), so the caller can clear its handle. */
static int PumpTx(cq_transport **txp, int isPoll)
{
    cq_transport *tx = *txp;
    cq_tx_status st = cq_tx_poll(tx);
    if (isPoll) gLastStat = (int)st;
    if (st == CQ_TX_DONE) {
        size_t len = 0;
        const unsigned char *d = cq_tx_data(tx, &len);
        AdoptReply(d, len);
        cq_tx_free(tx); *txp = NULL;
        return 1;
    } else if (st == CQ_TX_FAILED) {
        if (isPoll) gLastErr = (int)cq_tx_error_code(tx);
        strncpy(gLastMsg, cq_tx_error_message(tx), sizeof(gLastMsg) - 1);
        gLastMsg[sizeof(gLastMsg) - 1] = '\0';
        gOnline = false;
        cq_backoff_fail(&gBackoff);
        cq_tx_free(tx); *txp = NULL;
        Redraw();
        return 1;
    }
    return 0;
}

/* Fire a command over its own transaction (separate from the poll). Drops if a
 * command is already in flight — the poll reconciles. */
static void StartCommand(const char *sel)
{
    if (gCmd) return;
    gCmd = cq_tx_new(CQ_HOST, CQ_PORT, sel);
    if (gCmd) cq_tx_start(gCmd);
}

/* Decode JPEG cover bytes into a 64x64 GWorld via QuickTime's GraphicsImporter.
 * Robust: any failure just leaves the previous cover (or none) in place. */
static void DecodeCover(const unsigned char *bytes, size_t len)
{
    Handle jpeg = NULL, dataRef = NULL;
    GraphicsImportComponent gi = 0;
    GWorldPtr gw = NULL, savePort = NULL;
    GDHandle  saveGD = NULL;
    Rect r;
    SetRect(&r, 0, 0, CQ_COVER_PX, CQ_COVER_PX);

    jpeg = NewHandle((Size)len);
    if (!jpeg) return;
    BlockMoveData(bytes, *jpeg, (Size)len);

    if (PtrToHand(&jpeg, &dataRef, sizeof(Handle)) != noErr) { DisposeHandle(jpeg); return; }
    if (GetGraphicsImporterForDataRef(dataRef, 'hndl', &gi) != noErr || !gi) {
        DisposeHandle(dataRef); DisposeHandle(jpeg); return;
    }

    GetGWorld(&savePort, &saveGD);
    if (NewGWorld(&gw, 32, &r, NULL, NULL, 0) == noErr && gw) {
        PixMapHandle pm = GetGWorldPixMap(gw);
        LockPixels(pm);
        SetGWorld(gw, NULL);
        EraseRect(&r);
        GraphicsImportSetGWorld(gi, gw, NULL);
        GraphicsImportSetBoundsRect(gi, &r);
        GraphicsImportDraw(gi);
        UnlockPixels(pm);
    }
    SetGWorld(savePort, saveGD);

    CloseComponent(gi);
    DisposeHandle(dataRef);
    DisposeHandle(jpeg);

    if (gw) {
        if (gCoverGW) DisposeGWorld(gCoverGW);
        gCoverGW = gw;
    }
}

/* Advance the live poll + command path. Called every loop pass; never blocks. */
static void PollNetwork(void)
{
    if (gCmd) PumpTx(&gCmd, 0);

    /* Cover fetch (Fio 5): its reply is JPEG, not /now, so pump it separately. */
    if (gCover) {
        cq_tx_status st = cq_tx_poll(gCover);
        if (st == CQ_TX_DONE) {
            size_t len = 0;
            const unsigned char *d = cq_tx_data(gCover, &len);
            if (cq_data_is_jpeg(d, len)) {   /* law 7: sniff FF D8 before decoding */
                DecodeCover(d, len);
                Redraw();
            }
            strncpy(gCoverAlbum, gCoverReq, sizeof(gCoverAlbum) - 1);  /* tried; don't refetch */
            gCoverAlbum[sizeof(gCoverAlbum) - 1] = '\0';
            cq_tx_free(gCover); gCover = NULL;
        } else if (st == CQ_TX_FAILED) {
            cq_tx_free(gCover); gCover = NULL;
        }
    }
    /* Start a cover fetch when the album changes (and QuickTime is available). */
    if (gQTOk && gHaveSnap && !gCover && gSnap.album_id &&
        strcmp(gSnap.album_id, gCoverAlbum) != 0) {
        char sel[96];
        strncpy(gCoverReq, gSnap.album_id, sizeof(gCoverReq) - 1);
        gCoverReq[sizeof(gCoverReq) - 1] = '\0';
        snprintf(sel, sizeof(sel), "/spot/api/1/cover/%s/%d", gCoverReq, CQ_COVER_PX);
        gCover = cq_tx_new(CQ_HOST, CQ_PORT, sel);
        if (gCover) cq_tx_start(gCover);
    }

    /* Flush a debounced prev/next once it settles (law 1: coalesce before wire). */
    if (cq_debounce_has(&gDeb) && (long)(TickCount() - gCmdFire) >= 0 && !gCmd) {
        char *sel = cq_debounce_take(&gDeb);
        if (sel) { StartCommand(sel); free(sel); }
    }

    if (gTx) { PumpTx(&gTx, 1); return; }

    {
        unsigned long now = (unsigned long)TickCount();
        if (now - gLastPoll >= (unsigned long)cq_backoff_interval(&gBackoff)) {
            gLastPoll = now;                              /* the loop is the only clock */
            gTx = cq_tx_new(CQ_HOST, CQ_PORT, "/spot/api/1/now");
            if (gTx) { cq_tx_start(gTx); gPolls++; }
        }
    }
}

/* -------------------------------------------------------------------------- */

/* Create the transport controls (themed by the Appearance Manager). */
static void MakeControls(WindowRef win)
{
    Rect r;
    short W = ((GrafPtr)win)->portRect.right - ((GrafPtr)win)->portRect.left;

    SetRect(&r,  16, 156,  92, 176); gPrev = NewControl(win, &r, "\pPrev", true, 0, 0, 0, pushButProc, 0);
    SetRect(&r, 100, 156, 196, 176); gPlay = NewControl(win, &r, "\pPlay", true, 0, 0, 0, pushButProc, 0);
    SetRect(&r, 204, 156, 264, 176); gNext = NewControl(win, &r, "\pNext", true, 0, 0, 0, pushButProc, 0);
    /* Volume slider 0..100. */
    SetRect(&r, 300, 160, W - 16, 176);
    gVol = NewControl(win, &r, "\p", true, 50, 0, 100, kControlSliderProc, 0);
}

/* A click in the window content: transport buttons, volume slider, or a seek
 * on the progress bar. */
static void DoContentClick(WindowRef win, Point where)
{
    ControlHandle ctl;
    short W = ((GrafPtr)win)->portRect.right - ((GrafPtr)win)->portRect.left;
    Rect  bar;

    SetPort((GrafPtr)win);
    GlobalToLocal(&where);

    if (FindControl(where, win, &ctl) && ctl) {
        if (TrackControl(ctl, where, NULL)) {
            if (ctl == gPrev) {              /* debounced (law 1) */
                cq_debounce_set(&gDeb, "/spot/api/1/prev");
                gCmdFire = (unsigned long)TickCount() + CQ_DEBOUNCE_TICKS;
            } else if (ctl == gNext) {
                cq_debounce_set(&gDeb, "/spot/api/1/next");
                gCmdFire = (unsigned long)TickCount() + CQ_DEBOUNCE_TICKS;
            } else if (ctl == gPlay) {       /* immediate toggle */
                StartCommand(gSnap.state == CQ_STATE_PLAYING ?
                             "/spot/api/1/pause" : "/spot/api/1/play");
            } else if (ctl == gVol) {        /* commit on mouse-up, then hold (law 4) */
                char sel[48];
                snprintf(sel, sizeof(sel), "/spot/api/1/volume?%d", GetControlValue(gVol));
                gVolHold = (unsigned long)TickCount() + CQ_HOLD_TICKS;
                StartCommand(sel);
            }
        }
        return;
    }

    /* Seek: click on the progress bar to jump there. */
    SetRect(&bar, 16, 108, W - 16, 124);
    if (gHaveSnap && gSnap.duration_ms > 0 && PtInRect(where, &bar)) {
        long span = bar.right - bar.left;
        long frac = where.h - bar.left;
        long long ms;
        char sel[48];
        if (frac < 0) frac = 0;
        if (frac > span) frac = span;
        ms = (long long)gSnap.duration_ms * frac / span;
        snprintf(sel, sizeof(sel), "/spot/api/1/seek?%ld", (long)ms);
        StartCommand(sel);
    }
}

static void SetUpMenus(void)
{
    Handle mbar = GetNewMBar(kMenuBar);
    MenuHandle apple;
    SetMenuBar(mbar);
    apple = GetMenuHandle(kAppleMenu);
    if (apple) AppendResMenu(apple, 'DRVR');
    DrawMenuBar();
}

static void DoMenu(long choice)
{
    short menu = CQ_HIWORD(choice);
    short item = CQ_LOWORD(choice);
    Str255 daName;

    switch (menu) {
        case kAppleMenu:
            if (item == kAboutItem) {
                Alert(kAboutAlert, NULL);
            } else {
                MenuHandle am = GetMenuHandle(kAppleMenu);
                GetMenuItemText(am, item, daName);
                OpenDeskAcc(daName);
            }
            break;
        case kFileMenu:
            if (item == kQuitItem) gRunning = false;
            break;
    }
    HiliteMenu(0);
}

static void DoMouseDown(EventRecord *ev)
{
    WindowRef win;
    short part = FindWindow(ev->where, &win);

    switch (part) {
        case inMenuBar:   DoMenu(MenuSelect(ev->where));            break;
        case inSysWindow: SystemClick(ev, win);                    break;
        case inDrag:      DragWindow(win, ev->where, NULL);        break;
        case inGoAway:    if (TrackGoAway(win, ev->where)) gRunning = false; break;
        case inContent:
            if (win != FrontWindow()) SelectWindow(win);
            else DoContentClick(win, ev->where);
            break;
    }
}

int main(void)
{
    EventRecord ev;
    GDHandle    gd;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();

    RegisterAppearanceClient();               /* opt into the Platinum look */

    gQTOk = (EnterMovies() == noErr);         /* QuickTime for cover-art decode */

    {   /* UTF-8 -> MacRoman converter for the draw boundary */
        TextEncoding utf8 = CreateTextEncoding(kTextEncodingUnicodeV2_0,
                                kTextEncodingDefaultVariant, kUnicodeUTF8Format);
        if (TECCreateConverter(&gConv, utf8, kTextEncodingMacRoman) != noErr)
            gConv = NULL;
    }

    gd = GetMainDevice();                      /* depth/color for theme text */
    if (gd) {
        gDepth   = (**(**gd).gdPMap).pixelSize;
        gIsColor = TestDeviceAttribute(gd, gdDevType);
    }

    cq_guard_init(&gGuard);
    cq_backoff_init(&gBackoff, CQ_POLL_TICKS, CQ_POLL_CAP);
    cq_debounce_init(&gDeb);
    memset(&gSnap, 0, sizeof(gSnap));

    SetUpMenus();

    gWindow = GetNewWindow(kMainWindow, NULL, (WindowPtr)-1);
    if (gWindow) {
        SetThemeWindowBackground(gWindow, kThemeBrushDialogBackgroundActive, false);
        SetPort((GrafPtr)gWindow);
        MakeControls(gWindow);
        ShowWindow(gWindow);
    }

    while (gRunning) {
        /* Short sleep while a fetch is in flight (spin the OT state machine),
         * calmer when idle between polls. */
        long sleep = gTx ? 1L : 10L;
        if (WaitNextEvent(everyEvent, &ev, sleep, NULL)) {
            switch (ev.what) {
                case mouseDown:
                    DoMouseDown(&ev);
                    break;
                case keyDown:
                case autoKey:
                    if (ev.modifiers & cmdKey)
                        DoMenu(MenuKey((char)(ev.message & charCodeMask)));
                    break;
                case updateEvt: {
                    WindowRef win = (WindowRef)ev.message;
                    BeginUpdate(win);
                    DrawWindowContents(win);
                    EndUpdate(win);
                    break;
                }
                default:
                    break;
            }
        }
        PollNetwork();

        {   /* animate the diagnostics ~2x/sec so it's visible the loop is alive */
            unsigned long now = (unsigned long)TickCount();
            if (now - gLastDraw >= 30) { gLastDraw = now; Redraw(); }
        }
    }

    if (gTx) { cq_tx_cancel(gTx); cq_tx_free(gTx); }
    cq_now_free(&gSnap);
    FlushEvents(everyEvent, -1);
    return 0;
}
