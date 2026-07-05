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
#include <Folders.h>
#include <Files.h>
#include <Lists.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "cq_codec.h"
#include "cq_now.h"
#include "cq_guard.h"
#include "cq_backoff.h"
#include "cq_debounce.h"
#include "cq_track.h"
#include "cq_pls.h"
#include "cq_transport.h"

enum {
    kMenuBar     = 128,
    kAppleMenu   = 128,
    kFileMenu    = 129,
    kAboutAlert  = 128,
    kMainWindow  = 128,
    kPrefsDialog = 129,
    kAboutItem   = 1,
    kSearchItem  = 1,    /* File menu */
    kQueueItem   = 2,
    kListenItem  = 3,
    kPrefsItem   = 5,    /* Search, Queue, Listen, -, Preferences, -, Quit */
    kQuitItem    = 7
};

#define CQ_BUILD_TAG "b7"  /* bump on every VM-iteration build (see status row) */
#define CQ_DEFAULT_HOST "10.0.100.112"  /* server address is a pref (Fio 6) */
#define CQ_DEFAULT_PORT 70
#define CQ_POLL_TICKS 120           /* 2 s at 60 ticks/sec (law 5: >= micro-cache) */
#define CQ_POLL_CAP   1800          /* back off to at most 30 s on errors (429) */

/* HiWord/LoWord aren't in the interfaces uniformly; compute them. */
#define CQ_HIWORD(x) ((short)((x) >> 16))
#define CQ_LOWORD(x) ((short)((x) & 0xFFFF))

static Boolean       gRunning = true;
static WindowRef     gWindow  = NULL;
static char          gHost[64] = CQ_DEFAULT_HOST;   /* server address (pref) */
static int           gPort     = CQ_DEFAULT_PORT;
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

/* --- search + queue windows (Fio 7) --- */
static WindowRef     gSearchWin = NULL;
static ListHandle    gSearchList = NULL;
static ControlHandle gSearchEdit = NULL;
static cq_transport *gSearchTx = NULL;
static cq_track_list gSearchItems;
static WindowRef     gQueueWin = NULL;
static ListHandle    gQueueList = NULL;
static cq_transport *gQueueTx = NULL;
static cq_track_list gQueueItems;
static unsigned long gQueueLast = 0;        /* last /queue fetch (ticks) */
#define CQ_QUEUE_POLL_TICKS 300             /* re-fetch every 5 s while the window is open */
/* Double-click row i in the queue = skip forward i+1 times: the Web API has no
 * "jump to queue position" (nor reorder/remove — the queue is append-only), so
 * the jump is expressed as chained /next commands, fired one at a time. */
static short gSkipsPending = 0;
static ControlHandle gSearchBtn = NULL;
static ControlHandle gQueueAddBtn = NULL;   /* "Add to Queue" on the selected search row */

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
    /* CQ_BUILD_TAG: bumped per VM-iteration build so it's provable WHICH binary
     * is running over there (stale copies on the netatalk share bite). */
    snprintf(line, sizeof(line), "%s      %s      [%s]",
             gSnap.state == CQ_STATE_PLAYING ? "Playing" :
             gSnap.state == CQ_STATE_PAUSED  ? "Paused"  : "Stopped",
             gSnap.device == CQ_DEV_ACTIVE ? "on gopher-spot" :
             gSnap.device == CQ_DEV_IDLE   ? "playing elsewhere" : "",
             CQ_BUILD_TAG);
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
    gCmd = cq_tx_new(gHost, gPort, sel);
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
        gCover = cq_tx_new(gHost, gPort, sel);
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
            gTx = cq_tx_new(gHost, gPort, "/spot/api/1/now");
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

/* --- search + queue windows (Fio 7) --- */

/* Percent-escape into out. keepColon leaves ':' raw (Spotify URIs want it). */
static void EscInto(const char *s, char *out, size_t max, int keepColon)
{
    static const char *hx = "0123456789ABCDEF";
    size_t o = 0;
    for (; *s && o + 3 < max; s++) {
        unsigned char c = (unsigned char)*s;
        int safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (keepColon && c == ':') safe = 1;
        if (safe) out[o++] = (char)c;
        else { out[o++] = '%'; out[o++] = hx[c >> 4]; out[o++] = hx[c & 15]; }
    }
    out[o] = '\0';
}

/* Replace a list's rows with "track - artist" for each item. */
static void FillList(ListHandle list, cq_track_list *items)
{
    size_t i;
    Cell cell;
    if (!list) return;
    LDelRow(0, 0, list);
    for (i = 0; i < items->count; i++) {
        char row[256], mac[260];
        short len;
        snprintf(row, sizeof(row), "%s - %s",
                 items->items[i].track  ? items->items[i].track  : "?",
                 items->items[i].artist ? items->items[i].artist : "");
        len = ToMacRoman(row, mac, sizeof(mac));
        LAddRow(1, (short)i, list);
        SetPt(&cell, 0, (short)i);
        LSetCell(mac, len, cell, list);
    }
    {   /* InvalRect targets the CURRENT port; this runs from PumpAux, where the
         * port is whatever was drawn last (usually gWindow) — aim it at the
         * list's own window or the redraw lands in the wrong one. */
        GrafPtr save;
        GetPort(&save);
        SetPort((*list)->port);
        InvalRect(&(*list)->rView);
        SetPort(save);
    }
}

static ListHandle MakeList(WindowRef win, short top, short bottom)
{
    Rect view, bounds;
    Point csize;
    short W = ((GrafPtr)win)->portRect.right - ((GrafPtr)win)->portRect.left;
    short H = ((GrafPtr)win)->portRect.bottom - ((GrafPtr)win)->portRect.top;
    ListHandle l;
    SetRect(&view, 8, top, W - 8 - 15, H - bottom); /* room for the vertical scrollbar */
    /* dataBounds: columns = right-left, rows = bottom-top -> ONE column, zero
     * rows (rows come from LAddRow). 0,0,0,1 was zero columns: LSetCell had no
     * cell (0,i) to write, so every list rendered empty. */
    SetRect(&bounds, 0, 0, 1, 0);
    SetPt(&csize, 0, 0);
    l = LNew(&view, &bounds, csize, 0, win, true, false, false, true);
    return l;
}

/* Fire a selector and discard the reply (for /spot/play + queue/add, which
 * return a gophermap or /queue, NOT a /now — must not go through AdoptReply). */
static cq_transport *gFire = NULL;
static void StartFire(const char *sel)
{
    if (gFire) return;
    gFire = cq_tx_new(gHost, gPort, sel);
    if (gFire) cq_tx_start(gFire);
}

static void PlayItem(cq_track_list *items, int row)
{
    char euri[128], sel[160];
    if (row < 0 || (size_t)row >= items->count || !items->items[row].uri) return;
    EscInto(items->items[row].uri, euri, sizeof(euri), 1);   /* keep the colons */
    snprintf(sel, sizeof(sel), "/spot/play?uri=%s", euri);
    StartFire(sel);
}

static void OpenQueue(void)
{
    Rect r;
    if (gQueueWin) { SelectWindow(gQueueWin); }
    else {
        SetRect(&r, 96, 96, 96 + 320, 96 + 260);
        gQueueWin = NewCWindow(NULL, &r, "\pQueue", true, documentProc, (WindowPtr)-1, true, 0);
        SetPort((GrafPtr)gQueueWin);
        gQueueList = MakeList(gQueueWin, 8, 8);
    }
    if (!gQueueTx) {
        gQueueTx = cq_tx_new(gHost, gPort, "/spot/api/1/queue");
        if (gQueueTx) cq_tx_start(gQueueTx);
    }
}

static void RunSearch(void)
{
    char q[128], eq[400], sel[440];
    Size got = 0;
    if (!gSearchEdit) return;
    GetControlData(gSearchEdit, kControlNoPart, kControlEditTextTextTag,
                   sizeof(q) - 1, (Ptr)q, &got);
    if (got < 0) got = 0;
    q[got] = '\0';
    if (!q[0]) return;
    EscInto(q, eq, sizeof(eq), 0);
    snprintf(sel, sizeof(sel), "/spot/api/1/search?q=%s", eq);
    if (gSearchTx) { cq_tx_cancel(gSearchTx); cq_tx_free(gSearchTx); }
    gSearchTx = cq_tx_new(gHost, gPort, sel);
    if (gSearchTx) cq_tx_start(gSearchTx);
}

static void OpenSearch(void)
{
    Rect r, er, br;
    ControlHandle root;
    if (gSearchWin) { SelectWindow(gSearchWin); return; }
    SetRect(&r, 116, 116, 116 + 340, 116 + 280);
    gSearchWin = NewCWindow(NULL, &r, "\pSearch", true, documentProc, (WindowPtr)-1, true, 0);
    SetPort((GrafPtr)gSearchWin);
    /* Appearance keyboard focus REQUIRES an embedding hierarchy: without a root
     * control SetKeyboardFocus fails (errNoRootControl) and the edit field can
     * never take keystrokes. Must precede the NewControl calls so they embed. */
    CreateRootControl(gSearchWin, &root);
    SetRect(&er, 8, 8, 340 - 8 - 70, 26);
    gSearchEdit = NewControl(gSearchWin, &er, "\p", true, 0, 0, 0, kControlEditTextProc, 0);
    SetRect(&br, 340 - 8 - 64, 6, 340 - 8, 26);
    gSearchBtn = NewControl(gSearchWin, &br, "\pSearch", true, 0, 0, 0, pushButProc, 0);
    /* Bottom row: enqueue the selected result (double-click still = play now). */
    SetRect(&br, 8, 280 - 28, 8 + 110, 280 - 8);
    gQueueAddBtn = NewControl(gSearchWin, &br, "\pAdd to Queue", true, 0, 0, 0, pushButProc, 0);
    gSearchList = MakeList(gSearchWin, 36, 36);   /* leave room for the button row */
    /* Focus the field on open so ⌘F -> type -> Return just works. */
    if (gSearchEdit) SetKeyboardFocus(gSearchWin, gSearchEdit, kControlFocusNextPart);
}

/* Enqueue the selected search row (`/queue/add`, fire-and-forget). The reply is
 * a /queue snapshot we discard; PumpAux refreshes the queue window when the
 * fire completes. Eventually consistent (~1-2 s), like every command. */
static void QueueSelected(void)
{
    Cell c;
    char euri[128], sel[192];
    SetPt(&c, 0, 0);
    if (!gSearchList || !LGetSelect(true, &c, gSearchList)) return;
    if (c.v < 0 || (size_t)c.v >= gSearchItems.count || !gSearchItems.items[c.v].uri) return;
    EscInto(gSearchItems.items[c.v].uri, euri, sizeof(euri), 1);   /* keep the colons */
    snprintf(sel, sizeof(sel), "/spot/api/1/queue/add?%s", euri);
    StartFire(sel);
}

/* Close one of the auxiliary windows, freeing its list + items. */
static void CloseAux(WindowRef win)
{
    if (win == gQueueWin) {
        if (gQueueList) LDispose(gQueueList);
        cq_track_list_free(&gQueueItems);
        if (gQueueTx) { cq_tx_cancel(gQueueTx); cq_tx_free(gQueueTx); gQueueTx = NULL; }
        DisposeWindow(win);
        gQueueWin = NULL; gQueueList = NULL;
    } else if (win == gSearchWin) {
        if (gSearchList) LDispose(gSearchList);
        cq_track_list_free(&gSearchItems);
        if (gSearchTx) { cq_tx_cancel(gSearchTx); cq_tx_free(gSearchTx); gSearchTx = NULL; }
        DisposeWindow(win);
        gSearchWin = NULL; gSearchList = NULL; gSearchEdit = NULL; gSearchBtn = NULL;
        gQueueAddBtn = NULL;
    }
}

/* Click inside a list window: a control (search field/button) or the list. */
static void AuxClick(WindowRef win, Point where)
{
    ControlHandle ctl;
    ListHandle list = (win == gQueueWin) ? gQueueList : gSearchList;
    cq_track_list *items = (win == gQueueWin) ? &gQueueItems : &gSearchItems;

    SetPort((GrafPtr)win);
    GlobalToLocal(&where);

    if (win == gSearchWin && FindControl(where, win, &ctl) && ctl) {
        if (ctl == gSearchBtn) { if (TrackControl(ctl, where, NULL)) RunSearch(); }
        else if (ctl == gQueueAddBtn) { if (TrackControl(ctl, where, NULL)) QueueSelected(); }
        else if (ctl == gSearchEdit) {
            /* Appearance edit-text needs explicit keyboard focus — without it
             * HandleControlKey delivers keystrokes to an unfocused control and
             * typing never lands. */
            SetKeyboardFocus(win, ctl, kControlFocusNextPart);
            HandleControlClick(ctl, where, 0, NULL);
        }
        return;
    }
    if (list && LClick(where, 0, list)) {     /* double-click a row */
        Cell c;
        SetPt(&c, 0, 0);
        if (LGetSelect(true, &c, list)) {
            if (win == gQueueWin) {
                /* Jump WITHIN the queue: skip forward to the row, consuming it
                 * on the way (play?uri= here would leave a duplicate behind).
                 * Ignored while a previous jump is still draining. */
                if (gSkipsPending == 0 && c.v >= 0 && (size_t)c.v < items->count) {
                    gSkipsPending = (short)(c.v + 1);
                    StartFire("/spot/api/1/next");
                }
            } else {
                PlayItem(items, c.v);
            }
        }
    }
}

/* --- audio: live Icecast MP3 via QuickTime (the deferred, hardest piece) ---
 * Best-effort: discover the stream URL from /spot/stream.pls (cq_pls), then open
 * it as a QuickTime URL movie and service it from the loop with MoviesTask.
 * Classic-QuickTime streaming of a never-ending Icecast feed is finicky; this is
 * NOT runtime-verified and may need iteration on the VM. */
static Movie         gMovie = NULL;
static cq_transport *gPls   = NULL;
static int           gMovieLoading = 0;
static unsigned long gMovieStart = 0;

static void StopAudio(void)
{
    if (gMovie) { StopMovie(gMovie); DisposeMovie(gMovie); gMovie = NULL; }
    gMovieLoading = 0;
}

static void OpenStreamURL(const char *url)
{
    Handle urlH;
    Movie  mov = NULL;
    short  resID = 0;
    OSErr  e;
    size_t n = strlen(url);

    StopAudio();
    urlH = NewHandle((Size)n + 1);
    if (!urlH) return;
    BlockMoveData(url, *urlH, (Size)n + 1);            /* the URL C string, NUL included */
    /* ASYNC: NewMovieFromDataRef returns immediately; we poll the load state from
     * the loop (ServiceAudio). A synchronous open of a never-ending Icecast stream
     * blocks — and on a cooperative OS that FREEZES THE WHOLE MACHINE. Never do
     * that (NOTES.md). */
    e = NewMovieFromDataRef(&mov, newMovieActive | newMovieAsyncOK, &resID,
                            urlH, URLDataHandlerSubType);
    DisposeHandle(urlH);
    if (e != noErr || !mov) return;
    SetMovieVolume(mov, kFullVolume);
    gMovie = mov;
    gMovieLoading = 1;
    gMovieStart = (unsigned long)TickCount();
}

/* Service the audio movie from the event loop; never blocks. */
static void ServiceAudio(void)
{
    if (!gMovie) return;
    MoviesTask(gMovie, 0);                              /* 0 = do a little, return now */
    if (gMovieLoading) {
        long ls = GetMovieLoadState(gMovie);
        if (ls >= kMovieLoadStatePlaythroughOK) {
            StartMovie(gMovie);                        /* enough buffered — play */
            gMovieLoading = 0;
        } else if (ls == kMovieLoadStateError ||
                   (long)((unsigned long)TickCount() - gMovieStart) > 900) {  /* ~15 s */
            StopAudio();                               /* give up rather than hang */
        }
    }
}

/* Listen / Stop toggle. */
static void ToggleListen(void)
{
    if (!gQTOk) return;
    if (gMovie) { StopAudio(); return; }
    if (gPls) return;
    gPls = cq_tx_new(gHost, gPort, "/spot/stream.pls");   /* discover the stream URL */
    if (gPls) cq_tx_start(gPls);
}

/* Advance the search/queue/fire transactions; parse a list reply and fill it. */
static void PumpAux(void)
{
    if (gFire && cq_tx_poll(gFire) != CQ_TX_RUNNING) {
        cq_tx_free(gFire); gFire = NULL;
        if (gSkipsPending > 0) gSkipsPending--;
        if (gSkipsPending > 0) {
            /* Mid-jump: chain the next /next; hold the queue refresh until the
             * whole jump has drained (each hop would refetch pointlessly). */
            StartFire("/spot/api/1/next");
        } else if (gQueueWin && !gQueueTx) {
            /* A command (play / queue-add / end of a jump) just landed: if the
             * queue window is up, re-fetch it so the change shows without a
             * close/reopen. Eventually consistent (~1-2 s), so a very fast
             * refresh can still miss it — the 5 s poll catches up. */
            gQueueTx = cq_tx_new(gHost, gPort, "/spot/api/1/queue");
            if (gQueueTx) cq_tx_start(gQueueTx);
        }
    }

    ServiceAudio();                                       /* non-blocking audio service */

    if (gPls) {
        cq_tx_status st = cq_tx_poll(gPls);
        if (st == CQ_TX_DONE) {
            size_t len = 0;
            const unsigned char *d = cq_tx_data(gPls, &len);
            char *body = (char *)malloc(len + 1);
            if (body) {
                char *url;
                memcpy(body, d, len); body[len] = '\0';
                url = cq_pls_first_url(body);             /* PLS/M3U -> first stream URL */
                free(body);
                if (url) { OpenStreamURL(url); free(url); }
            }
            cq_tx_free(gPls); gPls = NULL;
        } else if (st == CQ_TX_FAILED) { cq_tx_free(gPls); gPls = NULL; }
    }

    if (gSearchTx) {
        cq_tx_status st = cq_tx_poll(gSearchTx);
        if (st == CQ_TX_DONE) {
            size_t len = 0;
            const unsigned char *d = cq_tx_data(gSearchTx, &len);
            cq_track_list_free(&gSearchItems);
            cq_track_list_from_response(&gSearchItems, d, len);
            FillList(gSearchList, &gSearchItems);
            cq_tx_free(gSearchTx); gSearchTx = NULL;
        } else if (st == CQ_TX_FAILED) { cq_tx_free(gSearchTx); gSearchTx = NULL; }
    }
    if (gQueueTx) {
        cq_tx_status st = cq_tx_poll(gQueueTx);
        if (st == CQ_TX_DONE) {
            size_t len = 0;
            const unsigned char *d = cq_tx_data(gQueueTx, &len);
            cq_track_list_free(&gQueueItems);
            cq_track_list_from_response(&gQueueItems, d, len);
            FillList(gQueueList, &gQueueItems);
            cq_tx_free(gQueueTx); gQueueTx = NULL;
            gQueueLast = (unsigned long)TickCount();
        } else if (st == CQ_TX_FAILED) {
            cq_tx_free(gQueueTx); gQueueTx = NULL;
            gQueueLast = (unsigned long)TickCount();
        }
    } else if (gQueueWin &&
               (unsigned long)TickCount() - gQueueLast >= CQ_QUEUE_POLL_TICKS) {
        /* Keep an open Queue window live (~5 s): the queue changes underneath
         * us — adds from the search window, tracks being consumed, and
         * Spotify's "idle player reads as empty" quirk all resolve on the
         * next poll instead of requiring a close/reopen. */
        gQueueTx = cq_tx_new(gHost, gPort, "/spot/api/1/queue");
        if (gQueueTx) cq_tx_start(gQueueTx);
    }
}

/* --- preferences: the server address (Fio 6) --- */

static void C2P(const char *c, Str255 p)
{
    size_t n = strlen(c);
    if (n > 255) n = 255;
    p[0] = (unsigned char)n;
    memcpy(p + 1, c, n);
}

static void P2C(ConstStr255Param p, char *c, size_t max)
{
    size_t n = p[0];
    if (n > max - 1) n = max - 1;
    memcpy(c, p + 1, n);
    c[n] = '\0';
}

static void PrefsSpec(FSSpec *spec)
{
    short vRef; long dirID;
    if (FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder,
                   &vRef, &dirID) == noErr)
        FSMakeFSSpec(vRef, dirID, "\pCasquinha Prefs", spec);
}

static void LoadPrefs(void)
{
    FSSpec spec;
    short  ref;
    long   count = 200;
    char   buf[256];
    char  *nl;

    PrefsSpec(&spec);
    if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) return;
    FSRead(ref, &count, buf);                 /* count comes back = bytes read */
    FSClose(ref);
    if (count <= 0) return;
    buf[count] = '\0';
    nl = strchr(buf, '\n');
    if (nl) {
        int p;
        *nl = '\0';
        if (buf[0]) { strncpy(gHost, buf, sizeof(gHost) - 1); gHost[sizeof(gHost) - 1] = '\0'; }
        p = atoi(nl + 1);
        if (p > 0 && p < 65536) gPort = p;
    }
}

static void SavePrefs(void)
{
    FSSpec spec;
    short  ref;
    long   count;
    char   buf[128];

    PrefsSpec(&spec);
    FSpCreate(&spec, 'Casq', 'TEXT', smSystemScript);   /* harmless if it exists */
    if (FSpOpenDF(&spec, fsWrPerm, &ref) != noErr) return;
    snprintf(buf, sizeof(buf), "%s\n%d\n", gHost, gPort);
    count = (long)strlen(buf);
    SetEOF(ref, 0);
    FSWrite(ref, &count, buf);
    FSClose(ref);
}

/* Preferences dialog: edit host + port, save, and reconnect. */
static void DoPrefs(void)
{
    DialogPtr d = GetNewDialog(kPrefsDialog, NULL, (WindowPtr)-1);
    short  item, type;
    Handle h;
    Rect   box;
    Str255 ps;
    char   pbuf[16];

    if (!d) return;

    GetDialogItem(d, 3, &type, &h, &box);              /* host edit field */
    C2P(gHost, ps); SetDialogItemText(h, ps);
    GetDialogItem(d, 5, &type, &h, &box);              /* port edit field */
    snprintf(pbuf, sizeof(pbuf), "%d", gPort);
    C2P(pbuf, ps); SetDialogItemText(h, ps);
    SelectDialogItemText(d, 3, 0, 32767);

    do { ModalDialog(NULL, &item); } while (item != 1 && item != 2);

    if (item == 1) {                                   /* Save */
        char host[64]; int p;
        GetDialogItem(d, 3, &type, &h, &box); GetDialogItemText(h, ps); P2C(ps, host, sizeof(host));
        GetDialogItem(d, 5, &type, &h, &box); GetDialogItemText(h, ps); P2C(ps, pbuf, sizeof(pbuf));
        p = atoi(pbuf);
        if (host[0]) { strncpy(gHost, host, sizeof(gHost) - 1); gHost[sizeof(gHost) - 1] = '\0'; }
        if (p > 0 && p < 65536) gPort = p;
        SavePrefs();
        cq_guard_reset(&gGuard);                       /* reconnect: fresh mark, poll now */
        cq_backoff_init(&gBackoff, CQ_POLL_TICKS, CQ_POLL_CAP);
        gLastPoll = 0;
        gCoverAlbum[0] = '\0';                          /* refetch cover from the new host */
    }
    DisposeDialog(d);
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
            if (item == kSearchItem)      OpenSearch();
            else if (item == kQueueItem)  OpenQueue();
            else if (item == kListenItem) ToggleListen();
            else if (item == kPrefsItem)  DoPrefs();
            else if (item == kQuitItem)   gRunning = false;
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
        case inDrag: {
            /* Classic InterfaceLib needs a REAL bounds rect — NULL is a Carbon
             * nicety; here it constrains the drag to nothing and the windows
             * can't be moved at all. Use the full desktop (all monitors). */
            Rect limit = (*GetGrayRgn())->rgnBBox;
            InsetRect(&limit, 4, 4);
            DragWindow(win, ev->where, &limit);
            break;
        }
        case inGoAway:
            if (TrackGoAway(win, ev->where)) {
                if (win == gWindow) gRunning = false;
                else CloseAux(win);
            }
            break;
        case inContent:
            if (win != FrontWindow()) SelectWindow(win);
            else if (win == gWindow)  DoContentClick(win, ev->where);
            else                      AuxClick(win, ev->where);
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
    LoadPrefs();                              /* server address from the prefs file */
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
                case autoKey: {
                    char ch = (char)(ev.message & charCodeMask);
                    if (ev.modifiers & cmdKey) {
                        DoMenu(MenuKey(ch));
                    } else if (FrontWindow() == gSearchWin && gSearchEdit) {
                        if (ch == '\r' || ch == '\n') RunSearch();
                        else {
                            /* Same port trap as IdleControls: HandleControlKey
                             * draws the typed character into the CURRENT port,
                             * which Redraw() leaves on gWindow — set it to the
                             * search window or keystrokes never appear. */
                            GrafPtr save;
                            GetPort(&save);
                            SetPort((GrafPtr)gSearchWin);
                            HandleControlKey(gSearchEdit,
                                 (short)((ev.message & keyCodeMask) >> 8), ch, ev.modifiers);
                            SetPort(save);
                        }
                    }
                    break;
                }
                case updateEvt: {
                    WindowRef win = (WindowRef)ev.message;
                    BeginUpdate(win);
                    if (win == gWindow) {
                        DrawWindowContents(win);
                    } else {
                        ListHandle list = (win == gQueueWin) ? gQueueList :
                                          (win == gSearchWin) ? gSearchList : NULL;
                        SetPort((GrafPtr)win);
                        SetThemeWindowBackground(win, kThemeBrushDialogBackgroundActive, false);
                        EraseRect(&((GrafPtr)win)->portRect);
                        UpdateControls(win, ((GrafPtr)win)->visRgn);
                        if (list) { LUpdate(((GrafPtr)win)->visRgn, list); FrameRect(&(*list)->rView); }
                    }
                    EndUpdate(win);
                    break;
                }
                default:
                    break;
            }
        }
        PollNetwork();
        PumpAux();
        if (gSearchWin) {   /* blink the search caret */
            /* IdleControls draws into the CURRENT port, and Redraw() leaves it
             * on gWindow — without this the caret blinks into the main window
             * (over the diagnostics) and the search field never gets one. */
            GrafPtr save;
            GetPort(&save);
            SetPort((GrafPtr)gSearchWin);
            IdleControls(gSearchWin);
            SetPort(save);
        }

        {   /* animate the diagnostics ~2x/sec so it's visible the loop is alive */
            unsigned long now = (unsigned long)TickCount();
            if (now - gLastDraw >= 30) { gLastDraw = now; Redraw(); }
        }
    }

    StopAudio();
    if (gTx) { cq_tx_cancel(gTx); cq_tx_free(gTx); }
    cq_now_free(&gSnap);
    FlushEvents(everyEvent, -1);
    return 0;
}
