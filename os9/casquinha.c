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

#include <string.h>
#include <stdio.h>

#include "cq_codec.h"
#include "cq_now.h"
#include "cq_guard.h"
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

/* HiWord/LoWord aren't in the interfaces uniformly; compute them. */
#define CQ_HIWORD(x) ((short)((x) >> 16))
#define CQ_LOWORD(x) ((short)((x) & 0xFFFF))

static Boolean       gRunning = true;
static WindowRef     gWindow  = NULL;
static short         gMonaco  = 4;

static cq_guard      gGuard;
static cq_now        gSnap;              /* current adopted snapshot */
static Boolean       gHaveSnap = false;
static Boolean       gOnline   = false;
static cq_transport *gTx       = NULL;   /* in-flight poll, or NULL */
static unsigned long gLastPoll = 0;

/* -------------------------------------------------------------------------- */

static void DrawCStr(short x, short y, const char *s)
{
    MoveTo(x, y);
    DrawText((Ptr)s, 0, (short)strlen(s));
}

static void DrawWindowContents(WindowRef win)
{
    char line[128];

    SetPort((GrafPtr)win);
    EraseRect(&((GrafPtr)win)->portRect);

    TextFont(gMonaco);
    TextSize(12);
    DrawCStr(16, 28, "Casquinha - gopher-spot on Mac OS 9");

    TextSize(10);

    if (!gHaveSnap) {
        DrawCStr(16, 60, gOnline ? "connecting..." : "connecting to 10.0.100.112:70 ...");
        return;
    }

    {
        const char *word = gSnap.state == CQ_STATE_PLAYING ? "> playing" :
                           gSnap.state == CQ_STATE_PAUSED  ? "|| paused" : "[] stopped";
        long long secs = gSnap.position_ms / 1000;
        long long dur  = gSnap.duration_ms / 1000;

        DrawCStr(16, 58, word);
        DrawCStr(16, 80, gSnap.track  ? gSnap.track  : "(no track)");
        DrawCStr(16, 96, gSnap.artist ? gSnap.artist : "");
        DrawCStr(16, 112, gSnap.album ? gSnap.album : "");

        snprintf(line, sizeof(line), "%ld:%02ld / %ld:%02ld    vol %d    %s",
                 (long)(secs / 60), (long)(secs % 60),
                 (long)(dur / 60),  (long)(dur % 60),
                 gSnap.volume,
                 gSnap.device == CQ_DEV_ACTIVE ? "device active" :
                 gSnap.device == CQ_DEV_IDLE   ? "device idle"   : "device unknown");
        DrawCStr(16, 136, line);
    }

    TextSize(9);
    if (!gOnline)
        DrawCStr(16, 168, "offline - retrying");
    else
        DrawCStr(16, 168, CQ_HOST ":" "70   polling every 2s");
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
        if (st == CQ_TX_DONE) {
            size_t len = 0;
            const unsigned char *d = cq_tx_data(gTx, &len);
            cq_now tmp;
            cq_now_from_response(&tmp, d, len);
            if (cq_guard_accept_ts(&gGuard, tmp.ts)) {   /* law 2: adopt only if ts >= mark */
                cq_now_free(&gSnap);
                gSnap = tmp;
                gHaveSnap = true;
            } else {
                cq_now_free(&tmp);                        /* staler replica: drop */
            }
            gOnline = true;
            cq_tx_free(gTx); gTx = NULL;
            Redraw();
        } else if (st == CQ_TX_FAILED) {
            gOnline = false;                              /* keep last snapshot on screen */
            cq_tx_free(gTx); gTx = NULL;
            Redraw();
        }
        return;
    }

    {
        unsigned long now = (unsigned long)TickCount();
        if (now - gLastPoll >= CQ_POLL_TICKS) {           /* the loop is the only clock */
            gLastPoll = now;
            gTx = cq_tx_new(CQ_HOST, CQ_PORT, "/spot/api/1/now");
            if (gTx) cq_tx_start(gTx);
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
    Str255 monaco = "\pMonaco";

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();

    GetFNum(monaco, &gMonaco);
    if (gMonaco == 0) gMonaco = 4;

    cq_guard_init(&gGuard);
    memset(&gSnap, 0, sizeof(gSnap));

    SetUpMenus();

    gWindow = GetNewWindow(kMainWindow, NULL, (WindowPtr)-1);
    if (gWindow) {
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
    }

    if (gTx) { cq_tx_cancel(gTx); cq_tx_free(gTx); }
    cq_now_free(&gSnap);
    FlushEvents(everyEvent, -1);
    return 0;
}
