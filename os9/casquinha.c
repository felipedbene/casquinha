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
#include <Menus.h>
#include <Fonts.h>
#include <Events.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <ToolUtils.h>
#include <Devices.h>
#include <Timer.h>
#include <Appearance.h>

#include <string.h>
#include <stdio.h>

#include "cq_codec.h"
#include "cq_now.h"
#include "cq_guard.h"
#include "cq_backoff.h"
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

/* -------------------------------------------------------------------------- */

static void DrawCStr(short x, short y, const char *s)
{
    MoveTo(x, y);
    DrawText((Ptr)s, 0, (short)strlen(s));
}

/* Apply a theme text color at the current device depth. */
static void ThemeText(ThemeTextColor c) { SetThemeTextColor(c, gDepth, gIsColor); }

/* Right-align a C string ending at x=right, baseline y. */
static void DrawCStrRight(short right, short y, const char *s)
{
    short w = TextWidth((Ptr)s, 0, (short)strlen(s));
    DrawCStr(right - w, y, s);
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

    /* State / volume / device row. */
    TextFont(1); TextSize(10);
    ThemeText(kThemeTextColorDialogActive);
    snprintf(line, sizeof(line), "%s      Volume %d      %s",
             gSnap.state == CQ_STATE_PLAYING ? "Playing" :
             gSnap.state == CQ_STATE_PAUSED  ? "Paused"  : "Stopped",
             gSnap.volume,
             gSnap.device == CQ_DEV_ACTIVE ? "on gopher-spot" :
             gSnap.device == CQ_DEV_IDLE   ? "playing elsewhere" : "");
    DrawCStr(16, 166, line);

    /* A quiet status line only when something's wrong. */
    if (gLastMsg[0]) {
        TextFont(1); TextSize(9);
        ThemeText(kThemeTextColorDialogInactive);
        DrawCStr(16, 188, gLastMsg);
    }
}

static void Redraw(void)
{
    if (gWindow) DrawWindowContents(gWindow);
}

/* Advance the live poll. Called every loop pass; never blocks (OT is
 * non-blocking, driven from here per NOTES.md). */
static void PollNetwork(void)
{
    if (gTx) {
        cq_tx_status st = cq_tx_poll(gTx);
        gLastStat = (int)st;
        if (st == CQ_TX_DONE) {
            size_t len = 0;
            const unsigned char *d = cq_tx_data(gTx, &len);
            cq_fields f;
            const char *err;
            gLastLen = (long)len;
            gDone++;
            gOnline = true;                               /* we reached the server */

            /* Check for an `error` key FIRST (law 6 / API contract). An error
             * document (e.g. `error upstream` on a Spotify 429) must NOT be
             * adopted as a stopped snapshot — that would wipe a good one. */
            cq_fields_init(&f);
            cq_fields_parse(&f, d, len);
            err = cq_fields_get(&f, "error");
            if (err) {
                /* Friendly, calm status — an upstream 429 is the bridge being
                 * Spotify-rate-limited, not a client fault; we ease off and
                 * recover automatically. */
                if (strcmp(err, "upstream") == 0)
                    snprintf(gLastMsg, sizeof(gLastMsg), "Spotify busy (rate limited) - easing off");
                else
                    snprintf(gLastMsg, sizeof(gLastMsg), "server error: %s", err);
                cq_backoff_fail(&gBackoff);                 /* don't hammer a 429 */
            } else {
                cq_now tmp;
                cq_backoff_ok(&gBackoff);                   /* healthy: back to 2 s */
                cq_now_from_fields(&tmp, &f);
                if (cq_guard_accept_ts(&gGuard, tmp.ts)) {  /* law 2: ts >= mark */
                    cq_now_free(&gSnap);
                    gSnap = tmp;
                    gHaveSnap = true;
                    gSnapTick = (unsigned long)TickCount();  /* interpolation origin */
                    gLastMsg[0] = '\0';
                } else {
                    cq_now_free(&tmp);                       /* staler replica: drop */
                }
            }
            cq_fields_free(&f);
            cq_tx_free(gTx); gTx = NULL;
            Redraw();
        } else if (st == CQ_TX_FAILED) {
            gLastErr = (int)cq_tx_error_code(gTx);
            strncpy(gLastMsg, cq_tx_error_message(gTx), sizeof(gLastMsg) - 1);
            gLastMsg[sizeof(gLastMsg) - 1] = '\0';
            gOnline = false;                              /* keep last snapshot on screen */
            cq_backoff_fail(&gBackoff);                   /* transport failure: back off too */
            cq_tx_free(gTx); gTx = NULL;
            Redraw();
        }
        return;
    }

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
        case inContent:   if (win != FrontWindow()) SelectWindow(win); break;
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

    gd = GetMainDevice();                      /* depth/color for theme text */
    if (gd) {
        gDepth   = (**(**gd).gdPMap).pixelSize;
        gIsColor = TestDeviceAttribute(gd, gdDevType);
    }

    cq_guard_init(&gGuard);
    cq_backoff_init(&gBackoff, CQ_POLL_TICKS, CQ_POLL_CAP);
    memset(&gSnap, 0, sizeof(gSnap));

    SetUpMenus();

    gWindow = GetNewWindow(kMainWindow, NULL, (WindowPtr)-1);
    if (gWindow) {
        SetThemeWindowBackground(gWindow, kThemeBrushDialogBackgroundActive, false);
        SetPort((GrafPtr)gWindow);
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
