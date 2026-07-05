/*
 * casquinha.c — the Mac OS 9 app shell (Fio 3, first slice).
 *
 * A classic PowerPC Toolbox application: a document window, a menu bar
 * (Apple / File ▸ Quit), and a WaitNextEvent loop. It renders a now-playing
 * snapshot in Monaco — parsed by the REAL pure core (cq_codec → cq_now), so this
 * .bin proves the ported layers compile and run on PowerPC and that the QuickDraw
 * render path works, before the Open Transport wire is added.
 *
 * Networking is not here yet: the Multiversal interfaces this toolchain ships
 * with have no Open Transport, so the live wire (cq_transport_ot.c) waits for the
 * Apple Universal Interfaces. For now the snapshot is a baked-in /now fixture,
 * fed through the exact same code the live path will use.
 *
 * Built with Retro68's classic-PPC toolchain (retroppc.toolchain.cmake).
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
#include <Sound.h>

#include <string.h>
#include <stdio.h>

#include "cq_codec.h"
#include "cq_now.h"

enum {
    kMenuBar    = 128,
    kAppleMenu  = 128,
    kFileMenu   = 129,
    kAboutAlert = 128,
    kMainWindow = 128,
    kAboutItem  = 1,
    kQuitItem   = 1
};

/* A baked /now snapshot (ASCII, so no MacRoman conversion needed yet). Same
 * bytes shape the wire delivers; parsed by the real cq_codec/cq_now. */
static const char *kNowFixture =
    "api\t1\r\nstate\tplaying\r\ntrack\tMamma Mia\r\nartist\tABBA\r\n"
    "album\tAbba\r\nposition_ms\t3100\r\nduration_ms\t213266\r\n"
    "device\tactive\r\nvolume\t100\r\nqueue_len\t20\r\nts\t1783131226118\r\n";

static Boolean   gRunning = true;
static WindowRef gWindow  = NULL;
static short     gMonaco  = 4;   /* Monaco font id (resolved at startup) */

/* HiWord/LoWord aren't in the Multiversal interfaces; compute them. */
#define CQ_HIWORD(x) ((short)((x) >> 16))
#define CQ_LOWORD(x) ((short)((x) & 0xFFFF))

/* MoveTo (x,y) then draw a C string with the current font. */
static void DrawCStr(short x, short y, const char *s)
{
    MoveTo(x, y);
    DrawText((Ptr)s, 0, (short)strlen(s));
}

static void DrawState(short x, short y, cq_play_state st)
{
    const char *glyph = st == CQ_STATE_PLAYING ? ">" :
                        st == CQ_STATE_PAUSED  ? "||" : "[]";
    char line[64];
    const char *word = st == CQ_STATE_PLAYING ? "playing" :
                       st == CQ_STATE_PAUSED  ? "paused"  : "stopped";
    snprintf(line, sizeof(line), "%s  %s", glyph, word);
    DrawCStr(x, y, line);
}

static void DrawWindowContents(WindowRef win)
{
    cq_now n;
    char line[128];
    long long secs, dur;

    SetPort((GrafPtr)win);
    EraseRect(&((GrafPtr)win)->portRect);   /* clear to white */

    cq_now_from_response(&n, (const unsigned char *)kNowFixture, strlen(kNowFixture));

    TextFont(gMonaco);
    TextSize(12);
    DrawCStr(16, 28, "Casquinha - gopher-spot on Mac OS 9");

    TextSize(10);
    DrawState(16, 58, n.state);

    DrawCStr(16, 78, n.track  ? n.track  : "(no track)");
    DrawCStr(16, 94, n.artist ? n.artist : "");
    DrawCStr(16, 110, n.album ? n.album : "");

    secs = n.position_ms / 1000;
    dur  = n.duration_ms / 1000;
    snprintf(line, sizeof(line), "%ld:%02ld / %ld:%02ld    vol %d    %s",
             (long)(secs / 60), (long)(secs % 60),
             (long)(dur / 60),  (long)(dur % 60),
             n.volume,
             n.device == CQ_DEV_ACTIVE ? "device active" :
             n.device == CQ_DEV_IDLE   ? "device idle"   : "device unknown");
    DrawCStr(16, 134, line);

    TextSize(9);
    DrawCStr(16, 162, "(baked snapshot - live Open Transport wire is next)");

    cq_now_free(&n);
}

static void SetUpMenus(void)
{
    Handle mbar = GetNewMBar(kMenuBar);
    MenuHandle apple;
    SetMenuBar(mbar);
    apple = GetMenuHandle(kAppleMenu);
    if (apple) AppendResMenu(apple, 'DRVR');   /* the desk accessories */
    DrawMenuBar();
}

static void DoAbout(void)
{
    Alert(kAboutAlert, NULL);
}

static void DoMenu(long choice)
{
    short menu = CQ_HIWORD(choice);
    short item = CQ_LOWORD(choice);
    Str255 daName;

    switch (menu) {
        case kAppleMenu:
            if (item == kAboutItem) {
                DoAbout();
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
        case inMenuBar:
            DoMenu(MenuSelect(ev->where));
            break;
        case inSysWindow:
            SystemClick(ev, win);
            break;
        case inDrag:
            DragWindow(win, ev->where, NULL);
            break;
        case inGoAway:
            if (TrackGoAway(win, ev->where)) gRunning = false;
            break;
        case inContent:
            if (win != FrontWindow()) SelectWindow(win);
            break;
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

    SetUpMenus();

    gWindow = GetNewWindow(kMainWindow, NULL, (WindowPtr)-1);
    if (gWindow) {
        SetPort((GrafPtr)gWindow);
        ShowWindow(gWindow);
    }

    while (gRunning) {
        if (WaitNextEvent(everyEvent, &ev, 30L, NULL)) {
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
                case mouseUp:
                default:
                    break;
            }
        }
    }

    FlushEvents(everyEvent, -1);
    return 0;
}
