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
#include <Sound.h>
#include <ImageCompression.h>
#include <QDOffscreen.h>
#include <Components.h>
#include <Folders.h>
#include <Files.h>
#include <Lists.h>

#include <Gestalt.h>
#include <AppleEvents.h>
#include <AERegistry.h>
#include <Multiprocessing.h>
#include <DriverServices.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "cq_codec.h"
#include "cq_now.h"
#include "cq_guard.h"
#include "cq_view.h"
#include "cq_backoff.h"
#include "cq_cache.h"
#include "cq_debounce.h"
#include "cq_track.h"
#include "cq_reflist.h"
#include "cq_pls.h"
#include "cq_mp3.h"
#include "cq_mp3dec.h"
#include "cq_decring.h"
#include "cq_transport.h"

enum {
    kMenuBar     = 128,
    kAppleMenu   = 128,
    kFileMenu    = 129,
    kAboutAlert  = 128,
    kMainWindow  = 128,
    kPrefsDialog = 129,
    kAboutItem   = 1,
    kPrefsItem   = 1,    /* File menu (b30): everything playback-related moved
                          * INTO the main window — menu tracking freezes the
                          * cooperative loop and starved the audio ring */
    kQuitItem    = 3     /* Preferences, -, Quit */
};

#define CQ_BUILD_TAG "b61"  /* bump on every VM-iteration build (see status row,
                             * the share filenames, and the per-build log name) */
#define CQ_DEFAULT_HOST "10.0.100.112"  /* server address is a pref (Fio 6) */
#define CQ_DEFAULT_PORT 70
#define CQ_POLL_TICKS 120           /* 2 s at 60 ticks/sec (law 5: >= micro-cache) */
#define CQ_POLL_CAP   1800          /* back off to at most 30 s on errors (429) */

/* HiWord/LoWord aren't in the interfaces uniformly; compute them. */
#define CQ_HIWORD(x) ((short)((x) >> 16))
#define CQ_LOWORD(x) ((short)((x) & 0xFFFF))

static Boolean       gRunning = true;
static Boolean       gSuspended = false; /* in background (Fio C): start no new polls */
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

/* The view (fio B): every word the player area shows — state word, the radio
 * status readout (b45/b49), the command ack (b48), the interpolated progress
 * — is rendered by the PURE cq_view module from one input struct; this glue
 * only gathers inputs and draws the output. gView is the last RENDERED truth
 * (Redraw re-renders before painting, so the screen never mixes clocks). */
static cq_view       gView;

/* The pending transport command (b48, generalized in fio B): captured at the
 * gesture, cleared when the snapshot reflects it or the 8 s settle window
 * times out — cq_view computes both; the glue just drops the note. */
static struct {
    cq_intent_kind kind;
    unsigned long  ticks;
    char           pre_track_id[24];
    cq_play_state  pre_state;
} gIntent;

static void SetIntent(cq_intent_kind kind)
{
    gIntent.kind  = kind;
    gIntent.ticks = (unsigned long)TickCount();
    gIntent.pre_track_id[0] = '\0';
    if (gHaveSnap && gSnap.track_id) {
        strncpy(gIntent.pre_track_id, gSnap.track_id,
                sizeof(gIntent.pre_track_id) - 1);
        gIntent.pre_track_id[sizeof(gIntent.pre_track_id) - 1] = '\0';
    }
    gIntent.pre_state = gHaveSnap ? gSnap.state : CQ_STATE_STOPPED;
}

/* The media plane's own fact (fio B / server fio A1): /spot/api/1/stream says
 * whether the Icecast mount carries real audio. Feature-detected ONCE per
 * launch (CLIENTS.md rule 24): 0 = unprobed, 1 = server has it, -1 =
 * not_found (old server — stop asking, cq_view keeps its rx heuristic). */
static cq_transport *gFactTx      = NULL;
static int           gStreamProbe = 0;
static int           gHaveFact    = 0;
static int           gFactLive    = 0;
static long long     gFactTs      = 0;
static unsigned long gFactLast    = 0;   /* last fetch tick */
static cq_backoff    gFBackoff;          /* lazy cadence, backs off on errors */
#define CQ_STREAM_POLL_TICKS 600         /* 10 s — a slow-moving fact (rule 23) */

/* --- debug log: OPT-IN via a marker file (b42). When a file named
 * "Casquinha Debug" sits next to the app, every DbgLog line goes to
 * "Casquinha <tag>.log" (flushed per line; copy back over the AFP share)
 * AND is mirrored live as one UDP datagram to the dev machine (b34 —
 * remote-syslog pattern: `make logtail` tails it in real time). The
 * marker's first line may override the mirror target as "host:port".
 * NO MARKER = NO TELEMETRY: no log file, no datagrams — a clean build for
 * anyone else's Mac. Best-effort everywhere: a logger must never get in
 * the way. */
#define CQ_LOG_HOST "10.0.1.165"   /* default mirror target (dev Mac) */
#define CQ_LOG_PORT 5514
static short gLogRef  = 0;   /* 0 = not open yet */
static int   gLogMode = 0;   /* 0 = marker unchecked, -1 = off, 1 = on */
static char  gLogHost[64] = CQ_LOG_HOST;
static int   gLogPort     = CQ_LOG_PORT;

static void DbgLog(const char *fmt, ...)
{
    char          buf[300];
    int           n;
    long          cnt;
    unsigned long t;
    va_list       ap;

    if (gLogMode < 0) return;                            /* telemetry is OFF */
    if (gLogMode == 0) {
        /* first call: does the "Casquinha Debug" marker exist? */
        FSSpec msp;
        if (FSMakeFSSpec(0, 0, "\pCasquinha Debug", &msp) != noErr) {
            gLogMode = -1;                               /* no marker: stay silent */
            return;
        }
        {   /* optional first line = "host:port" mirror override */
            short mref;
            if (FSpOpenDF(&msp, fsRdPerm, &mref) == noErr) {
                char mb[64];
                long mc = sizeof(mb) - 1;
                FSRead(mref, &mc, mb);                   /* eofErr still fills mc */
                FSClose(mref);
                if (mc > 0) {
                    char *e, *colon;
                    mb[mc] = '\0';
                    for (e = mb; *e && *e != '\r' && *e != '\n'; e++) {}
                    *e = '\0';
                    colon = strchr(mb, ':');
                    if (colon) {
                        int p = atoi(colon + 1);
                        *colon = '\0';
                        if (p > 0 && p < 65536) gLogPort = p;
                    }
                    if (mb[0]) {
                        strncpy(gLogHost, mb, sizeof(gLogHost) - 1);
                        gLogHost[sizeof(gLogHost) - 1] = '\0';
                    }
                }
            }
        }
        gLogMode = 1;
    }
    if (!gLogRef) {
        FSSpec sp;
        OSErr  e;
        unsigned char pname[64];   /* "Casquinha <tag>.log": one log PER BUILD,
                                      so a VM session can never be misread
                                      against the wrong binary's log */
        int    pn = snprintf((char *)pname + 1, sizeof(pname) - 1,
                             "Casquinha %s.log", CQ_BUILD_TAG);
        pname[0] = (unsigned char)pn;
        e = FSMakeFSSpec(0, 0, pname, &sp);              /* app's folder */
        if (e != noErr && e != fnfErr) return;
        FSpCreate(&sp, 'ttxt', 'TEXT', smSystemScript);  /* harmless if it exists */
        if (FSpOpenDF(&sp, fsRdWrPerm, &gLogRef) != noErr) { gLogRef = 0; return; }
        SetFPos(gLogRef, fsFromLEOF, 0);                 /* append across runs */
    }
    t = (unsigned long)TickCount();
    n = snprintf(buf, sizeof(buf) - 2, "[%lu.%02lus] ", t / 60, (t % 60) * 100 / 60);
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - 2 - (size_t)n, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    buf[n++] = '\r';                                     /* classic line ending */
    cnt = n;
    FSWrite(gLogRef, &cnt, buf);
    FlushVol(NULL, 0);        /* commit now — the tail must survive a freeze */
    buf[n - 1] = '\n';        /* unix ending for the live tail */
    cq_tx_udp(gLogHost, gLogPort, buf, (size_t)n);
}

/* Freeze-probe sink (b60): the transport hands us a preformatted line when one
 * of its un-deadlined synchronous OT provider traps runs slow — route it to the
 * timestamped, FlushVol'd log so a stall lands on disk (and the UDP tail) with
 * its trap name and elapsed ticks. */
static void TxProbeLog(const char *msg)
{
    DbgLog("%s", msg);
}

/* --- transport controls (Fio 4) --- */
static ControlHandle gPrev = NULL, gPlay = NULL, gNext = NULL, gVol = NULL;
static cq_debounce   gDeb;               /* pre-wire coalescer for prev/next (law 1) */
static unsigned long gCmdFire = 0;       /* tick to flush the debounced command */
static cq_transport *gCmd = NULL;        /* in-flight command transaction */
static unsigned long gVolHold = 0;       /* ignore poll volume until this tick (law 4) */

#define CQ_DEBOUNCE_TICKS 18             /* ~0.3 s settle before a prev/next reaches the wire */
#define CQ_HOLD_TICKS    180             /* ~3 s single hold window on the volume slider */

/* --- cover art (Fio 5; memory discipline from Fio A) --- */
#define CQ_COVER_PX 64
static Boolean       gQTOk    = false;   /* QuickTime available (EnterMovies ok) */
static cq_transport *gCover   = NULL;    /* in-flight cover fetch */
/* album_id -> decoded GWorld; an entry with a NULL GWorld means "tried, no
 * image" (failed fetch / not JPEG), so it is never re-requested this run
 * (CLIENTS.md checklist 6). 64x64x32bpp = 16 KB per decoded slot, <=128 KB. */
static cq_cache      gCovers;
static char          gCoverReq[64]   = "";  /* album_id being fetched */

/* --- connection budget (Fio E) --- */
#define CQ_MAX_INFLIGHT 3      /* automatic starters (cover, queue poll) wait above this */
static int TxInFlight(void);   /* defined after the last transport global (gPls) */

/* --- search + queue, IN the main window (Fio 7; promoted from separate
 * windows in b30 — menu/window juggling starved the audio ring, and the
 * queue deserves to be a glance away). Lists + controls live in gWindow. */
static ListHandle    gSearchList = NULL;
static ControlHandle gSearchEdit = NULL;
static cq_transport *gSearchTx = NULL;
static cq_track_list gSearchItems;
/* Artists & albums in the SAME result list (b57): a search now surfaces three
 * kinds. The list renders artist rows, then album rows, then track rows; the
 * clicked row's index maps back to a kind via the three counts (ResultKind).
 * Clicking an artist DRILLS into its discography (gArtistTx -> the album list,
 * gAlbumBrowse=1); clicking an album plays the WHOLE thing (play/context);
 * a track plays as before. */
static cq_ref_list   gSearchArtists;        /* search artist.<i>.{id,name} */
static cq_ref_list   gSearchAlbums;         /* search album.<i>.{id,name} */
static cq_ref_list   gArtistAlbums;         /* one artist's discography (drilled in) — kept
                                              separate so "< back" restores the search results */
static cq_transport *gArtistTx = NULL;      /* /artist/<id>/albums drill in flight */
static int           gAlbumBrowse = 0;      /* 1 = list shows one artist's albums (drilled in) */
static char          gBrowseArtist[64] = "";/* the artist name we drilled into (status/back row) */
static ListHandle    gQueueList = NULL;
static cq_transport *gQueueTx = NULL;
static cq_track_list gQueueItems;
static unsigned long gQueueLast = 0;        /* last /queue fetch (ticks) */
#define CQ_QUEUE_POLL_TICKS 600             /* re-fetch every 10 s — the queue is
                                              ALWAYS visible now, so the poll is
                                              permanent: halve the old cadence per
                                              the exhaustion discipline (Fio E/H);
                                              the post-command kick keeps it live */
static cq_backoff    gQBackoff;             /* queue re-fetch cadence, backs off on errors (Fio D) */
static unsigned long gQueueKick = 0;        /* one-shot /queue refetch due at this tick (Fio H); 0 = none */
static int           gQueueEmptyRuns = 0;   /* consecutive empty /queue replies (b33: only 3 in a row
                                              are believed — idle/racing players read as empty) */
/* Double-click a queue row = play that track directly with one /spot/play?uri=
 * (see DoContentClick). The Web API has no jump-to-queue-index; chaining /next per
 * row was N rate-limited player calls and a visible FFwd, so one-call direct play
 * won out (tradeoff: the track may replay later — it isn't removed from queue). */
static ControlHandle gSearchBtn = NULL;
static ControlHandle gQueueAddBtn = NULL;   /* "Add to Queue" on the selected search row */
/* No Listen/Wake buttons since b46: auto-start (b43) made them ceremonial —
 * the app tunes and wakes itself at launch. ⌘T/⌘K and the AppleScript
 * commands remain as the manual levers. */

/* Shelf geometry (b30): the player area above CQ_SHELF_TOP is redrawn by the
 * 2 Hz animator; everything below is controls + List Manager territory and
 * only repaints on real update events (or the animator would wipe it). */
#define CQ_SHELF_TOP    232
#define CQ_LIST_TOP     282   /* b46: the dead button row went to the lists */
#define CQ_LIST_BOTTOM  474
#define CQ_LABEL_Y      276   /* breathing room above the -1-inset list frames */

/* -------------------------------------------------------------------------- */

/* The wire is UTF-8; QuickDraw draws MacRoman. Convert at the draw boundary only
 * (NOTES.md), via the Text Encoding Converter. ASCII passes through unchanged, so
 * headers/times/numbers are unaffected; accents (ç ã é ê õ) render correctly. */
static TECObjectRef gConv = NULL;      /* UTF-8 -> MacRoman (draw boundary) */
static TECObjectRef gConvOut = NULL;   /* MacRoman -> UTF-8 (wire boundary, Fio F) */

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

/* The inverse boundary (Fio F): edit-text controls hand back MacRoman, but the
 * wire is UTF-8 — percent-encoding raw MacRoman bytes mangles accents on the
 * server ("Construção" must leave as constru%C3%A7%C3%A3o). ASCII passes
 * through unchanged on the no-converter fallback. */
static void ToUTF8(const char *mac, char *dst, size_t dstmax)
{
    ByteCount srcLen = (ByteCount)strlen(mac), srcRead = 0, dstLen = 0;
    if (gConvOut) {
        OSStatus e = TECConvertText(gConvOut, (ConstTextPtr)mac, srcLen, &srcRead,
                                    (TextPtr)dst, (ByteCount)(dstmax - 1), &dstLen);
        if (e == noErr) { dst[dstLen] = '\0'; return; }
    }
    { size_t n = strlen(mac);
      if (n > dstmax - 1) n = dstmax - 1;
      memcpy(dst, mac, n); dst[n] = '\0'; }
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
    /* Platinum: fill with the theme window background, not raw white.
     * ONLY the player area — the shelf below (search/queue lists) is List
     * Manager territory and repaints on update events; erasing it from the
     * 2 Hz animator would blank the lists twice a second (b30). */
    SetRect(&pr, 0, 0, W, CQ_SHELF_TOP);
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
    if (gView.title_green && gIsColor) {
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

    /* Progress bar — native themed track; the interpolation (and its clamp)
     * is cq_view's, computed with the same clock as everything else drawn. */
    {
        long posMs = (long)gView.position_ms;
        long durMs = (long)gView.duration_ms;

        if (gView.show_progress) {
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
    /* State word — or the pending-command ack ("Skipping...", b48): both are
     * cq_view's call now, from the same render as everything else drawn. */
    snprintf(line, sizeof(line), "%s      %s      [%s]",
             gView.state_word,
             gSnap.device == CQ_DEV_ACTIVE ? "on gopher-spot" :
             gSnap.device == CQ_DEV_IDLE   ? "playing elsewhere" : "",
             CQ_BUILD_TAG);
    DrawCStr(16, 202, line);

    /* the engine narrating itself, right-aligned on the same row (b45/b49 —
     * the vocabulary lives in cq_view; TickView animates the count-up) */
    if (gView.status[0]) DrawCStrRight(W - 16, 202, gView.status);

    /* Album cover (Fio 5), top-right, drawn last so it sits above the text.
     * Looked up by the view's ONE key — never a stale neighbor's cover. */
    if (gView.cover_key[0]) {
        GWorldPtr gw = (GWorldPtr)cq_cache_get(&gCovers, gView.cover_key);
        if (gw) {
            PixMapHandle pm = GetGWorldPixMap(gw);
            Rect sr, dr;
            SetRect(&sr, 0, 0, CQ_COVER_PX, CQ_COVER_PX);
            SetRect(&dr, W - 16 - CQ_COVER_PX, 44, W - 16, 44 + CQ_COVER_PX);
            LockPixels(pm);
            CopyBits((BitMap *)*pm, &((GrafPtr)win)->portBits, &sr, &dr, srcCopy, NULL);
            UnlockPixels(pm);
        }
    }

    /* A quiet status line only when something's wrong. */
    if (gLastMsg[0]) {
        TextFont(1); TextSize(9);
        ThemeText(kThemeTextColorDialogInactive);
        DrawCStr(16, 220, gLastMsg);
    }
}

/* The static shelf furniture (b30): separator, list labels, list frames.
 * Drawn ONLY on real update events — the 2 Hz animator never touches it. */
static void DrawShelf(WindowRef win)
{
    Rect  pr = ((GrafPtr)win)->portRect;
    short W  = pr.right - pr.left;
    Rect  sep;

    SetPort((GrafPtr)win);
    /* fresh background for the shelf; the caller re-draws controls + lists */
    SetRect(&sep, 0, CQ_SHELF_TOP, W, pr.bottom);
    SetThemeWindowBackground(win, kThemeBrushDialogBackgroundActive, false);
    EraseRect(&sep);
    SetRect(&sep, 16, CQ_SHELF_TOP, W - 16, CQ_SHELF_TOP + 2);
    DrawThemeSeparator(&sep, kThemeStateActive);

    TextFont(1); TextSize(9);
    ThemeText(kThemeTextColorDialogInactive);
    DrawCStr(16, CQ_LABEL_Y, "Results");
    DrawCStr(W / 2 + 4, CQ_LABEL_Y, "Up Next");

    /* Frame the WHOLE list — rView excludes the scrollbar (LNew carves 15 px
     * off), so framing rView alone leaves the scrollbar floating outside the
     * box (b32 screenshot). Widen back over it, inset -1 for the border. */
    if (gSearchList) {
        Rect fr = (*gSearchList)->rView;
        fr.right += 15;
        InsetRect(&fr, -1, -1);
        FrameRect(&fr);
    }
    if (gQueueList) {
        Rect fr = (*gQueueList)->rView;
        fr.right += 15;
        InsetRect(&fr, -1, -1);
        FrameRect(&fr);
    }
}

/* Re-render the view, then paint: every repaint draws ONE render's output,
 * so the state word, status readout, ack and progress can never mix clocks.
 * (RenderView lives with the audio globals it reads, below.) */
static void RenderView(void);

static void Redraw(void)
{
    RenderView();
    if (gWindow) DrawWindowContents(gWindow);
}

/* Keep the Play/Pause button title in sync with the current state. */
static void UpdatePlayTitle(void)
{
    if (gPlay)
        SetControlTitle(gPlay, gSnap.state == CQ_STATE_PLAYING ? "\pPause" : "\pPlay");
}

static void StartCommand(const char *sel);   /* defined with the transport glue */
static void PlayItem(cq_track_list *items, int row);   /* search/queue section */
static void PlayFrom(cq_track_list *items, int row, cq_track_list *cont);
static void FillResults(void);                         /* artists+albums+tracks in gSearchList */
static void StartArtistAlbums(const char *artistId, const char *artistName);
static void PlayContextAlbum(const char *albumId);
/* Result rows are laid out artists, then albums, then tracks (normal search),
 * or a "back" row then albums (drilled into one artist). Map a list row to its
 * kind + index within that kind's list. */
typedef enum { CQ_ROW_ARTIST, CQ_ROW_ALBUM, CQ_ROW_TRACK, CQ_ROW_BACK, CQ_ROW_NONE } cq_row_kind;
static cq_row_kind ResultKindAt(int row, size_t *localIdx);
static void StartFire(const char *sel);
static void EscInto(const char *s, char *out, size_t max, int keepColon);
static int gAutoNexted = 0;   /* end-of-track watchdog: one auto-next per stop */

/* Native play-from (b41, SPEC-play-from.md): /spot/api/1/play/from?ids=…
 * hands Spotify a real multi-track context so advance/next/prev are native.
 * 0 = unprobed, 1 = server has it, -1 = not_found (fall back to the b40
 * single-uri + client-sequencer path). gFallbackUri holds the clicked
 * track's uri while a probe is in flight, so a not_found reply can replay
 * the user's intent the old way. */
static int  gNativePlay = 0;
static char gFallbackUri[128] = "";

/* Auto-start (b43): opening the app IS the intent to listen — tune the
 * stream at launch and fire ONE wake off the first /now snapshot when
 * playback isn't already on the gopher-spot device. Single-shot per launch
 * (CLIENTS.md rule 10 read honestly: launch = user intent, once); holding
 * the OPTION key at launch skips it all. */
static int gAutoStart = 1;
static int gAutoWoke  = 0;
static int gAutoPlayPending = 0;   /* stopped-at-launch: play the queue head
                                      once the first /queue snapshot lands */


/* The row AFTER the current track in the visible queue — the client-side
 * "play from here onward" cursor (b40). Spotify cannot jump into a queue, so
 * jumps play bare URIs and the server queue is never consumed; the visible
 * list therefore IS the playlist, and the app sequences it: find the current
 * track_id among the rows, return the next one. Not found -> the head (0);
 * nothing after it (or an empty list) -> -1, playback honestly ends. */
static int QueueNextRow(void)
{
    size_t i;
    if (gQueueItems.count == 0) return -1;
    if (!gSnap.track_id) return 0;
    for (i = 0; i < gQueueItems.count; i++) {
        const char *u  = gQueueItems.items[i].uri;
        const char *id = u ? strrchr(u, ':') : NULL;   /* "spotify:track:<id>" */
        if (id && strcmp(id + 1, gSnap.track_id) == 0)
            return ((i + 1) < gQueueItems.count) ? (int)(i + 1) : -1;
    }
    return 0;
}

/*
 * Adopt a /now-shaped reply from EITHER a poll or a command. Checks the error
 * key first (law 6), adopts a healthy snapshot through the ts-guard (laws 2/3),
 * and drives the backoff. A command's reply is authoritative — it comes through
 * the same guard, so no catch-up poll storm (law 3).
 */
static void AdoptReply(const unsigned char *d, size_t len, int isCmd)
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
        /* Switch on the CODE only; message text is not part of the contract.
         * rate_limited = the bridge sits inside Spotify's cooldown answering
         * from cache — keep the last snapshot AND the normal cadence
         * (CLIENTS.md g6, Fio G); backing off here only staled the UI. Every
         * other code (upstream, ...) backs the poll off as before. */
        if (strcmp(err, "rate_limited") == 0) {
            snprintf(gLastMsg, sizeof(gLastMsg), "Spotify busy - holding steady");
        } else if (isCmd && strcmp(err, "not_found") == 0 && gFallbackUri[0]) {
            /* The play/from probe hit an old server (b41): flip to the b40
             * path once and replay the user's intent as a single-uri play —
             * the client sequencer resumes duty. Not a poll failure: no
             * backoff. gFallbackUri is only ever set by PlayFrom, and
             * commands are serialized (StartCommand drops overlaps), so this
             * not_found is provably ours. */
            char euri[128], sel2[192];
            gNativePlay = -1;
            DbgLog("play/from: not_found - old server, single-uri fallback");
            EscInto(gFallbackUri, euri, sizeof(euri), 1);
            snprintf(sel2, sizeof(sel2), "/spot/play?uri=%s", euri);
            gFallbackUri[0] = '\0';
            StartFire(sel2);
        } else {
            snprintf(gLastMsg, sizeof(gLastMsg), "server error: %s", err);
            cq_backoff_fail(&gBackoff);
        }
    } else {
        if (isCmd && gFallbackUri[0]) {
            /* the play/from reply came back a healthy snapshot: native it is */
            if (gNativePlay == 0)
                DbgLog("play/from: server supports it - native contexts on");
            gNativePlay = 1;
            gFallbackUri[0] = '\0';
        }
        cq_now tmp;
        cq_backoff_ok(&gBackoff);
        cq_now_from_fields(&tmp, &f);
        if (cq_guard_accept_ts(&gGuard, tmp.ts)) {
            int volHeld = (long)((TickCount() - gVolHold)) < 0;  /* within hold window? */
            int keepVol = gSnap.volume;
            /* End-of-track watchdog (b35): one-call direct play (`play?uri=`)
             * gives Spotify a single-track context, and librespot STOPS at
             * its end instead of pulling the queue (b33: 2:20/2:20 frozen,
             * queue full, /now confirmed `stopped`). When playing-near-the-
             * end flips to stopped while our queue view has items, do what a
             * human would — ONE /next. Single-shot per stop; re-arms only
             * when playback actually resumes, so it can never storm the
             * rate-limited player endpoint. */
            if (tmp.state == CQ_STATE_STOPPED && !gAutoNexted &&
                gHaveSnap && gSnap.state == CQ_STATE_PLAYING &&
                gSnap.duration_ms > 0 &&
                gSnap.position_ms >= gSnap.duration_ms - 10000) {
                /* Sequence the VISIBLE queue: play the row after the track
                 * that just ended (b40 — "from here onward"), via a bare
                 * play?uri= (from a dead single-track context /next no-ops
                 * or NO_ACTIVE_DEVICEs, b35 field report). gSnap still holds
                 * the ended track here — adoption happens below. */
                int row = QueueNextRow();
                gAutoNexted = 1;
                if (row >= 0) {
                    DbgLog("watchdog: track ended - playing queue from row %d of %ld",
                           row, (long)gQueueItems.count);
                    PlayFrom(&gQueueItems, row, NULL);
                } else if (gQueueItems.count > 0) {
                    DbgLog("watchdog: track ended - end of visible queue");
                }
            } else if (tmp.state == CQ_STATE_PLAYING) {
                gAutoNexted = 0;
            }
            /* Track changed = the queue definitely moved (head consumed /
             * jump landed). Kick ONE queue re-poll now instead of waiting
             * out the 10 s cadence — event-driven freshness at ~one extra
             * poll per track (b39: the UI felt ~30 s behind). */
            if (tmp.track_id && (!gSnap.track_id ||
                                 strcmp(tmp.track_id, gSnap.track_id) != 0)) {
                gQueueKick = (unsigned long)TickCount() + 120;
                /* (the skip ack clears itself: cq_view sees the reflection) */
                /* the UI-flip moment, timestamped: ear-vs-UI staleness at
                 * natural transitions is measurable from the log sink (b44) */
                DbgLog("now: %s - %s",
                       tmp.track  ? tmp.track  : "?",
                       tmp.artist ? tmp.artist : "?");
            }
            {   /* Equal-ts re-adoption is a NO-OP for the progress anchor: a
                 * cache-window poll re-serves the same document, and moving
                 * the anchor rewound the bar ~2 s (fio A1 audit finding). */
                int sameTs = gHaveSnap && tmp.ts > 0 && tmp.ts == gSnap.ts;
                cq_now_free(&gSnap);
                gSnap = tmp;
                gHaveSnap = true;
                if (!sameTs) gSnapTick = (unsigned long)TickCount();
            }
            gLastMsg[0] = '\0';
            if (volHeld && gHaveSnap) gSnap.volume = keepVol;    /* don't yank a live drag */
            UpdatePlayTitle();
            if (gVol && !volHeld && gSnap.volume >= 0)
                SetControlValue(gVol, gSnap.volume);
            /* Auto-start off the FIRST snapshot (b43, matrix refined in b47
             * after a launch hickup: wake?play=1 on a PAUSED-but-active
             * device does NOT resume — transfer-to-self is a no-op, the b35
             * field dance). Decide like a human would:
             *   playing on the device  -> nothing;
             *   paused  on the device  -> plain resume (/play), no transfer;
             *   stopped on the device  -> the context is dead; play the
             *       visible queue head — but /queue hasn't landed yet at
             *       first-/now time, so defer via gAutoPlayPending;
             *   anywhere else / idle   -> wake?play=1 (transfer + play). */
            if (gAutoStart && !gAutoWoke) {
                gAutoWoke = 1;
                if (gSnap.device == CQ_DEV_ACTIVE) {
                    if (gSnap.state == CQ_STATE_PLAYING) {
                        DbgLog("auto: already playing on gopher-spot");
                    } else if (gSnap.state == CQ_STATE_PAUSED) {
                        DbgLog("auto: resume on launch (paused on device)");
                        SetIntent(CQ_INTENT_PLAY);
                        StartCommand("/spot/api/1/play");
                    } else {
                        DbgLog("auto: stopped on device - queue head once it loads");
                        gAutoPlayPending = 1;
                    }
                } else {
                    DbgLog("auto: wake on launch (state=%d dev=%d)",
                           (int)gSnap.state, (int)gSnap.device);
                    SetIntent(CQ_INTENT_PLAY);
                    StartCommand("/spot/api/1/wake?play=1");
                }
            }
            if (gAutoPlayPending && gSnap.state == CQ_STATE_PLAYING)
                gAutoPlayPending = 0;          /* it recovered by itself */
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
        AdoptReply(d, len, !isPoll);
        /* A command reply IS the fresh snapshot — push the next poll a full
         * interval out (CLIENTS.md checklist 4: no /now within ~1 s, Fio G). */
        if (!isPoll) gLastPoll = (unsigned long)TickCount();
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
/* --- UI action debounce (b59) ---------------------------------------------
 * A command's round-trip + server settle is ~0.5-2 s. With no instant feedback a
 * user re-clicks, and every extra click used to fire — drill x3, play/context x2,
 * a DOUBLE queue/album append (a real side effect). Swallow a repeat of the SAME
 * action within the guard window; DISTINCT actions are never blocked, so paced
 * deliberate actions stay snappy. Transport keeps its own coalescer (gDeb) and
 * volume its hold window, so this governs only the discrete list/button actions
 * (drill, play/context, play/from, queue/add, queue/album, search, play/pause). */
#define CQ_ACTION_GUARD_TICKS 40           /* ~0.67 s at 60 ticks/s */
static char          gLastAction[240] = "";
static unsigned long gLastActionTick  = 0;
static int ActionRepeat(const char *sel)
{
    unsigned long now = (unsigned long)TickCount();
    if (gLastAction[0] && strcmp(sel, gLastAction) == 0 &&
        (long)(now - gLastActionTick) < CQ_ACTION_GUARD_TICKS) {
        DbgLog("debounce: swallowed repeat %s", sel);
        return 1;
    }
    strncpy(gLastAction, sel, sizeof(gLastAction) - 1);
    gLastAction[sizeof(gLastAction) - 1] = '\0';
    gLastActionTick = now;
    return 0;
}

static void StartCommand(const char *sel)
{
    if (gCmd) return;
    gCmd = cq_tx_new(gHost, gPort, sel);
    if (gCmd) cq_tx_start(gCmd);
}

/* Decode JPEG cover bytes into a 64x64 GWorld via QuickTime's GraphicsImporter.
 * Returns the GWorld (caller owns it — DisposeGWorld) or NULL on any failure. */
static GWorldPtr DecodeCover(const unsigned char *bytes, size_t len)
{
    Handle jpeg = NULL, dataRef = NULL;
    GraphicsImportComponent gi = 0;
    GWorldPtr gw = NULL, savePort = NULL;
    GDHandle  saveGD = NULL;
    Rect r;
    SetRect(&r, 0, 0, CQ_COVER_PX, CQ_COVER_PX);

    jpeg = NewHandle((Size)len);
    if (!jpeg) return NULL;
    BlockMoveData(bytes, *jpeg, (Size)len);

    if (PtrToHand(&jpeg, &dataRef, sizeof(Handle)) != noErr) { DisposeHandle(jpeg); return NULL; }
    if (GetGraphicsImporterForDataRef(dataRef, 'hndl', &gi) != noErr || !gi) {
        DisposeHandle(dataRef); DisposeHandle(jpeg); return NULL;
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

    return gw;
}

/* Advance the live poll + command path. Called every loop pass; never blocks. */
static void PollNetwork(void)
{
    if (gCmd) PumpTx(&gCmd, 0);

    /* Cover fetch (Fio 5): its reply is JPEG, not /now, so pump it separately.
     * EVERY outcome — decoded, not-JPEG, transport failure — is recorded in
     * gCovers, so an album is requested at most once per run (Fio A: before
     * this, a FAILED fetch left no mark and was retried every loop pass). */
    if (gCover) {
        cq_tx_status st = cq_tx_poll(gCover);
        if (st != CQ_TX_RUNNING) {
            GWorldPtr gw = NULL, old;
            if (st == CQ_TX_DONE) {
                size_t len = 0;
                const unsigned char *d = cq_tx_data(gCover, &len);
                if (cq_data_is_jpeg(d, len))   /* law 7: sniff FF D8 before decoding */
                    gw = DecodeCover(d, len);
            }
            old = (GWorldPtr)cq_cache_put(&gCovers, gCoverReq, gw);
            if (old) DisposeGWorld(old);
            if (gw) Redraw();
            cq_tx_free(gCover); gCover = NULL;
        }
    }
    /* Start a cover fetch only for an album never tried this run — and only
     * in the foreground, under the connection budget (Fios C/E). */
    if (gQTOk && gHaveSnap && !gCover && !gSuspended &&
        TxInFlight() < CQ_MAX_INFLIGHT && gSnap.album_id &&
        !cq_cache_has(&gCovers, gSnap.album_id)) {
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

    if (!gSuspended) {   /* background starts no polls; resume kicks one (Fio C) */
        unsigned long now = (unsigned long)TickCount();
        /* seeded = interval + up to 25% positive jitter (Fio D): several
         * clients desynchronize, and the base never gets FASTER (law 5). */
        if (now - gLastPoll >= (unsigned long)cq_backoff_interval_seeded(&gBackoff, now)) {
            gLastPoll = now;                              /* the loop is the only clock */
            gTx = cq_tx_new(gHost, gPort, "/spot/api/1/now");
            if (gTx) { cq_tx_start(gTx); gPolls++; }
        }
    }
}

/* -------------------------------------------------------------------------- */

/* Create the transport controls (themed by the Appearance Manager). */
/* Shelf actions — defined with the search/queue code below. */
static ListHandle MakeList(WindowRef win, const Rect *area);
static void RunSearch(void);
static void QueueSelected(void);
static void ToggleListen(void);

/* Next that OBEYS what the screen shows (b37): /next assumes Spotify has a
 * live context to skip within — from a stopped/bare-URI state it no-ops or
 * fails NO_ACTIVE_DEVICE while the user stares at a full queue. If nothing
 * is playing and the visible queue has a head, play that head directly. */
static void NextCommand(void)
{
    SetIntent(CQ_INTENT_SKIP);   /* the click lands on screen NOW (b48) */
    if (gSnap.state != CQ_STATE_PLAYING) {
        int row = QueueNextRow();
        if (row >= 0) {
            DbgLog("next: player not playing - playing queue from row %d", row);
            PlayFrom(&gQueueItems, row, NULL);
            return;
        }
    }
    cq_debounce_set(&gDeb, "/spot/api/1/next");
    gCmdFire = (unsigned long)TickCount() + CQ_DEBOUNCE_TICKS;
}

static void MakeControls(WindowRef win)
{
    Rect r;
    short W = ((GrafPtr)win)->portRect.right - ((GrafPtr)win)->portRect.left;
    ControlHandle root;

    /* Appearance keyboard focus REQUIRES an embedding hierarchy: without a
     * root control SetKeyboardFocus fails (errNoRootControl) and the search
     * field can never take keystrokes. Must precede every NewControl. */
    CreateRootControl(win, &root);

    SetRect(&r,  16, 156,  92, 176); gPrev = NewControl(win, &r, "\pPrev", true, 0, 0, 0, pushButProc, 0);
    SetRect(&r, 100, 156, 196, 176); gPlay = NewControl(win, &r, "\pPlay", true, 0, 0, 0, pushButProc, 0);
    SetRect(&r, 204, 156, 264, 176); gNext = NewControl(win, &r, "\pNext", true, 0, 0, 0, pushButProc, 0);
    /* Volume slider 0..100. */
    SetRect(&r, 300, 160, W - 16, 176);
    gVol = NewControl(win, &r, "\p", true, 50, 0, 100, kControlSliderProc, 0);

    /* --- the shelf (b30/b46): search + queue, always visible. Menu tracking
     * freezes the cooperative loop (and thus starves the audio ring), so
     * everything you'd touch mid-listening lives HERE. One row: field +
     * Search + Add to Queue (Listen/Wake became automatic in b43). */
    SetRect(&r, 16, 240, W - 206, 262);
    gSearchEdit = NewControl(win, &r, "\p", true, 0, 0, 0, kControlEditTextProc, 0);
    SetRect(&r, W - 198, 238, W - 134, 260);
    gSearchBtn = NewControl(win, &r, "\pSearch", true, 0, 0, 0, pushButProc, 0);
    SetRect(&r, W - 126, 238, W - 16, 260);
    gQueueAddBtn = NewControl(win, &r, "\pAdd to Queue", true, 0, 0, 0, pushButProc, 0);

    SetRect(&r, 16, CQ_LIST_TOP, W / 2 - 6, CQ_LIST_BOTTOM);
    gSearchList = MakeList(win, &r);
    SetRect(&r, W / 2 + 4, CQ_LIST_TOP, W - 16, CQ_LIST_BOTTOM);
    gQueueList = MakeList(win, &r);
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
        if (ctl == gSearchEdit) {
            /* Appearance edit-text needs explicit keyboard focus — without it
             * HandleControlKey delivers keystrokes to an unfocused control and
             * typing never lands. */
            SetKeyboardFocus(win, ctl, kControlFocusNextPart);
            HandleControlClick(ctl, where, 0, NULL);
            return;
        }
        if (TrackControl(ctl, where, NULL)) {
            if (ctl == gPrev) {              /* debounced (law 1) */
                SetIntent(CQ_INTENT_SKIP);
                cq_debounce_set(&gDeb, "/spot/api/1/prev");
                gCmdFire = (unsigned long)TickCount() + CQ_DEBOUNCE_TICKS;
            } else if (ctl == gNext) {
                NextCommand();               /* obeys the visible queue (b37) */
            } else if (ctl == gPlay) {       /* immediate toggle (debounced, b59) */
                const char *ps = gSnap.state == CQ_STATE_PLAYING ?
                                 "/spot/api/1/pause" : "/spot/api/1/play";
                if (!ActionRepeat(ps)) {
                    SetIntent(gSnap.state == CQ_STATE_PLAYING ?
                              CQ_INTENT_PAUSE : CQ_INTENT_PLAY);
                    StartCommand(ps);
                }
            } else if (ctl == gVol) {        /* commit on mouse-up, then hold (law 4) */
                char sel[48];
                snprintf(sel, sizeof(sel), "/spot/api/1/volume?%d", GetControlValue(gVol));
                gVolHold = (unsigned long)TickCount() + CQ_HOLD_TICKS;
                StartCommand(sel);
            } else if (ctl == gSearchBtn) {
                RunSearch();
            } else if (ctl == gQueueAddBtn) {
                QueueSelected();
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
        return;
    }

    /* The lists: single click selects, double click plays. Both jump straight
     * to the clicked track with ONE /spot/play?uri= (PlayItem). The queue used
     * to chain N /next hops to consume the intervening rows and avoid a
     * duplicate, but that's N calls to the rate-limited player endpoint and a
     * visible FFwd per row; Spotify has no jump-to-queue-index, so one-call
     * direct play is the cheap choice. Tradeoff: the chosen track may remain
     * in up-next and replay later. */
    {
        ListHandle list = NULL;
        cq_track_list *items = NULL;
        if (gSearchList && PtInRect(where, &(*gSearchList)->rView)) {
            list = gSearchList; items = &gSearchItems;
        } else if (gQueueList && PtInRect(where, &(*gQueueList)->rView)) {
            list = gQueueList; items = &gQueueItems;
        }
        if (list && LClick(where, 0, list)) {     /* double-click a row */
            Cell c;
            SetPt(&c, 0, 0);
            if (LGetSelect(true, &c, list)) {
                /* Native contexts (b41): a queue row plays from there onward;
                 * a search hit plays now and continues with the queue. b57: a
                 * search-list row can also be an artist (drill into its albums)
                 * or an album (play the whole thing) — route by row kind. */
                if (list == gSearchList) {
                    size_t li = 0;
                    switch (ResultKindAt(c.v, &li)) {
                    case CQ_ROW_TRACK:
                        PlayFrom(&gSearchItems, (int)li, &gQueueItems);
                        break;
                    case CQ_ROW_ARTIST:
                        StartArtistAlbums(gSearchArtists.items[li].id,
                                          gSearchArtists.items[li].name);
                        break;
                    case CQ_ROW_ALBUM: {
                        cq_ref_list *src = gAlbumBrowse ? &gArtistAlbums : &gSearchAlbums;
                        if (li < src->count) PlayContextAlbum(src->items[li].id);
                        break;
                    }
                    case CQ_ROW_BACK:
                        gAlbumBrowse = 0;
                        FillResults();   /* back to the last search's results */
                        break;
                    default:
                        break;
                    }
                } else {
                    PlayFrom(items, c.v, NULL);
                }
            }
        }
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

/* Append one MacRoman-encoded row to gSearchList at row r. */
static void AddResultRow(ListHandle list, short r, const char *text)
{
    char mac[300];
    short len = ToMacRoman(text, mac, sizeof(mac));
    Cell cell;
    LAddRow(1, r, list);
    SetPt(&cell, 0, r);
    LSetCell(mac, len, cell, list);
}

/* Fill gSearchList with the current results. Normal search: artist rows, then
 * album rows, then track rows — each labelled so the gesture reads (tap an
 * artist to see its albums, an album to play the whole thing). Drilled into one
 * artist (gAlbumBrowse): a "< back" row, then that artist's albums. Mirrors
 * FillList's port dance so the invalidate lands in the list's own window. */
static void FillResults(void)
{
    ListHandle list = gSearchList;
    size_t i;
    short r = 0;
    if (!list) return;
    LDelRow(0, 0, list);

    if (gAlbumBrowse) {
        char back[256];
        snprintf(back, sizeof(back), "< %s", gBrowseArtist[0] ? gBrowseArtist : "voltar");
        AddResultRow(list, r++, back);
        for (i = 0; i < gArtistAlbums.count; i++) {
            char row[256];
            snprintf(row, sizeof(row), "  album: %s",
                     gArtistAlbums.items[i].name ? gArtistAlbums.items[i].name : "?");
            AddResultRow(list, r++, row);
        }
    } else {
        for (i = 0; i < gSearchArtists.count; i++) {
            char row[256];
            snprintf(row, sizeof(row), "artista: %s",
                     gSearchArtists.items[i].name ? gSearchArtists.items[i].name : "?");
            AddResultRow(list, r++, row);
        }
        for (i = 0; i < gSearchAlbums.count; i++) {
            char row[256];
            snprintf(row, sizeof(row), "album: %s",
                     gSearchAlbums.items[i].name ? gSearchAlbums.items[i].name : "?");
            AddResultRow(list, r++, row);
        }
        for (i = 0; i < gSearchItems.count; i++) {
            char row[256];
            snprintf(row, sizeof(row), "%s - %s",
                     gSearchItems.items[i].track  ? gSearchItems.items[i].track  : "?",
                     gSearchItems.items[i].artist ? gSearchItems.items[i].artist : "");
            AddResultRow(list, r++, row);
        }
    }
    {
        GrafPtr save;
        GetPort(&save);
        SetPort((*list)->port);
        InvalRect(&(*list)->rView);
        SetPort(save);
    }
}

/* Map a clicked list row to its kind + index within that kind's sub-list. Row
 * layout matches FillResults: artists|albums|tracks, or back|albums when drilled. */
static cq_row_kind ResultKindAt(int row, size_t *localIdx)
{
    size_t r;
    if (row < 0) return CQ_ROW_NONE;
    r = (size_t)row;
    if (gAlbumBrowse) {
        if (r == 0) return CQ_ROW_BACK;
        r -= 1;
        if (r < gArtistAlbums.count) { if (localIdx) *localIdx = r; return CQ_ROW_ALBUM; }
        return CQ_ROW_NONE;
    }
    if (r < gSearchArtists.count) { if (localIdx) *localIdx = r; return CQ_ROW_ARTIST; }
    r -= gSearchArtists.count;
    if (r < gSearchAlbums.count) { if (localIdx) *localIdx = r; return CQ_ROW_ALBUM; }
    r -= gSearchAlbums.count;
    if (r < gSearchItems.count) { if (localIdx) *localIdx = r; return CQ_ROW_TRACK; }
    return CQ_ROW_NONE;
}

/* Drill into an artist: fetch /spot/api/1/artist/<id>/albums. The reply (parsed
 * in the tx pump) becomes gSearchAlbums and flips the list into gAlbumBrowse. */
static void StartArtistAlbums(const char *artistId, const char *artistName)
{
    char sel[128];
    if (!artistId || !artistId[0]) return;
    snprintf(sel, sizeof(sel), "/spot/api/1/artist/%s/albums", artistId);
    if (ActionRepeat(sel)) return;   /* debounce repeat drill (b59) */
    if (gArtistTx) { cq_tx_cancel(gArtistTx); cq_tx_free(gArtistTx); gArtistTx = NULL; }
    strncpy(gBrowseArtist, artistName ? artistName : "", sizeof(gBrowseArtist) - 1);
    gBrowseArtist[sizeof(gBrowseArtist) - 1] = '\0';
    gArtistTx = cq_tx_new(gHost, gPort, sel);
    if (gArtistTx) cq_tx_start(gArtistTx);
    DbgLog("drill: artist %s albums", artistId);
}

/* Play a WHOLE album as a context — the native "queue this album" (one PUT,
 * Spotify owns the continuation). Its reply is a settled /now, so it goes
 * through StartCommand/AdoptReply like play/pause, and the intent gives cq_view
 * an instant ack. */
static void PlayContextAlbum(const char *albumId)
{
    char uri[80], euri[160], sel[224];
    if (!albumId || !albumId[0]) return;
    snprintf(uri, sizeof(uri), "spotify:album:%s", albumId);
    EscInto(uri, euri, sizeof(euri), 1);   /* keep the colons */
    snprintf(sel, sizeof(sel), "/spot/api/1/play/context?uri=%s", euri);
    if (ActionRepeat(sel)) return;   /* debounce repeat album play (b59) */
    SetIntent(CQ_INTENT_PLAY);
    StartCommand(sel);
    DbgLog("play/context: album %s", albumId);
}

static ListHandle MakeList(WindowRef win, const Rect *area)
{
    Rect view = *area, bounds;
    Point csize;
    ListHandle l;
    view.right -= 15;                       /* room for the vertical scrollbar */
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
    if (ActionRepeat(sel)) return;   /* debounce double-click / lag re-tap (b59) */
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

/* Native "from here onward" (b41, design/SPEC-play-from.md): send bare track
 * ids — the clicked row plus its continuation — to /spot/api/1/play/from,
 * handing Spotify a real multi-track context: advance/next/prev then behave
 * natively, no client sequencing. cont = the follow-on list (the queue after
 * a search hit), or NULL to continue with the rest of `items`. Capped at 24
 * ids (spec; geomyidae's request-line buffer). Old servers answer not_found
 * and AdoptReply replays the intent as a b40 single-uri play. */
static void PlayFrom(cq_track_list *items, int row, cq_track_list *cont)
{
    char   sel[900];
    size_t o, i;
    int    n = 0;
    cq_track_list *rest = cont ? cont : items;
    size_t start        = cont ? 0    : (size_t)row + 1;

    if (row < 0 || (size_t)row >= items->count || !items->items[row].uri) return;
    if (gNativePlay < 0) { PlayItem(items, row); return; }   /* known-old server */
    if (gCmd) return;                    /* commands are serialized (StartCommand) */

    o = (size_t)snprintf(sel, sizeof(sel), "/spot/api/1/play/from?ids=");
    {   /* the clicked row first... */
        const char *id = strrchr(items->items[row].uri, ':');
        if (!id || !id[1]) { PlayItem(items, row); return; }
        o += (size_t)snprintf(sel + o, sizeof(sel) - o, "%s", id + 1);
        n = 1;
    }
    /* ...then the continuation */
    for (i = start; i < rest->count && n < 24; i++) {
        const char *u  = rest->items[i].uri;
        const char *id = u ? strrchr(u, ':') : NULL;
        if (!id || !id[1]) continue;
        if (o + strlen(id + 1) + 16 >= sizeof(sel)) break;
        sel[o++] = ',';
        o += (size_t)snprintf(sel + o, sizeof(sel) - o, "%s", id + 1);
        n++;
    }
    snprintf(sel + o, sizeof(sel) - o, "&offset=0");

    strncpy(gFallbackUri, items->items[row].uri, sizeof(gFallbackUri) - 1);
    gFallbackUri[sizeof(gFallbackUri) - 1] = '\0';
    DbgLog("play/from: %d ids (row %d)%s", n, row,
           gNativePlay == 0 ? " [probe]" : "");
    if (ActionRepeat(sel)) return;   /* debounce repeat list play (b59) */
    StartCommand(sel);
}

static void RunSearch(void)
{
    char q[128], q8[384], eq[1160], sel[1200];
    Size got = 0;
    if (!gSearchEdit) return;
    GetControlData(gSearchEdit, kControlNoPart, kControlEditTextTextTag,
                   sizeof(q) - 1, (Ptr)q, &got);
    if (got < 0) got = 0;
    q[got] = '\0';
    if (!q[0]) return;
    ToUTF8(q, q8, sizeof(q8));       /* MacRoman field -> UTF-8 wire (Fio F) */
    EscInto(q8, eq, sizeof(eq), 0);
    snprintf(sel, sizeof(sel), "/spot/api/1/search?q=%s", eq);
    if (ActionRepeat(sel)) return;   /* debounce a repeat of the same query (b59) */
    /* A fresh search leaves any artist-drill (b57): clear browse mode and the
     * drilled discography so the reply repaints normal artist/album/track rows. */
    gAlbumBrowse = 0;
    gBrowseArtist[0] = '\0';
    if (gArtistTx) { cq_tx_cancel(gArtistTx); cq_tx_free(gArtistTx); gArtistTx = NULL; }
    if (gSearchTx) { cq_tx_cancel(gSearchTx); cq_tx_free(gSearchTx); }
    gSearchTx = cq_tx_new(gHost, gPort, sel);
    if (gSearchTx) cq_tx_start(gSearchTx);
}

/* Enqueue the selected search row (`/queue/add`, fire-and-forget). The reply is
 * a /queue snapshot we discard; PumpAux refreshes the queue list when the
 * fire completes. Eventually consistent (~1-2 s), like every command. */
static void QueueSelected(void)
{
    Cell c;
    char euri[128], sel[192];
    size_t li = 0;
    cq_row_kind k;
    SetPt(&c, 0, 0);
    if (!gSearchList || !LGetSelect(true, &c, gSearchList)) return;
    /* b57: a track row enqueues its one uri (queue/add); an ALBUM row enqueues
     * the whole album onto up-next (queue/album — the server expands it, since
     * Spotify's queue takes one uri at a time). Artist/back rows: nothing. */
    k = ResultKindAt(c.v, &li);
    if (k == CQ_ROW_TRACK) {
        if (li >= gSearchItems.count || !gSearchItems.items[li].uri) return;
        EscInto(gSearchItems.items[li].uri, euri, sizeof(euri), 1);   /* keep the colons */
        snprintf(sel, sizeof(sel), "/spot/api/1/queue/add?%s", euri);
        StartFire(sel);
    } else if (k == CQ_ROW_ALBUM) {
        cq_ref_list *src = gAlbumBrowse ? &gArtistAlbums : &gSearchAlbums;
        if (li >= src->count || !src->items[li].id) return;
        /* album ids are base62 (no escaping needed); server re-gates the id */
        snprintf(sel, sizeof(sel), "/spot/api/1/queue/album?id=%s", src->items[li].id);
        StartFire(sel);
        DbgLog("queue/album: %s", src->items[li].id);
    }
}

/* --- audio: live Icecast MP3 — OUR wire, QuickTime as codec only (b16) -----
 *
 * QT's URL data handler cannot open a length-less live stream: b13–b15 logs
 * show the MP3 importer parked at kMovieLoadStateLoading with 0 tracks for a
 * full minute (idle-import flag included) while our own OT probe pulled 87 KB
 * in 5 s from the same URL. So the Movie Toolbox is out of the audio path:
 *
 *   cq_tx_stream (endless OT read)  ->  HTTP header skip  ->  cq_mp3 sync
 *     ->  SoundConverter '.mp3' -> 'twos' PCM  ->  SndChannel bufferCmds
 *
 * Everything advances in bounded slices from the event loop. The ONLY
 * interrupt-time code is AudioBufDone flipping a buffer-free flag; the
 * converter runs in loop context and pulls compressed bytes via AudioFill. */
typedef enum { AU_IDLE = 0, AU_HTTP, AU_SYNC, AU_PLAY } au_state;

/* Output stage (b25): SndPlayDoubleBuffer + a PCM ring — the canonical OS 9
 * streaming architecture, adopted after the bufferCmd+callBackCmd bookkeeping
 * provably broke under pressure (b24 log: "played" outran wall-clock by 33%,
 * i.e. busy flags cleared early, queued buffers got overwritten with newer
 * audio = the chops-and-overlaps). Here the race is impossible by shape:
 * the interrupt-time double-back proc ONLY reads the ring, the event loop
 * ONLY writes it (single producer / single consumer, two cursors), and a
 * starved ring is served as SILENCE — the channel never dies, playback
 * resumes by itself when the loop refills (menus, drags, dialogs included).
 *
 * Radio physics (b23): a live mount arrives at exactly playback rate, so the
 * cushion never grows on its own — it is BUILT, up front: ~5 s of decoded
 * PCM before the first note. Startup latency IS the freeze immunity. */
#define CQ_AUD_RING_BYTES (2048L * 1024L)  /* ~11.9 s of 44.1 kHz stereo 16-bit */
#define CQ_AUD_CHUNK      (32L * 1024L)    /* per double-buffer refill (~0.19 s) */
/* Latency knobs (b29/b31, repriced in b53): the backlog IS both the
 * command→ear delay and the freeze/dry-spell immunity — one buys the other.
 * The server side (librespot→Icecast) adds its own ~1-2 s floor that no
 * client knob removes.
 *
 * b31 lesson: b29's "stop decoding at the target" cap did NOT cap latency —
 * the backlog just moved upstream into the staging (4 s) and transport (4 s)
 * buffers, invisible and un-trimmed: users felt ~10 s. Now the decoder always
 * runs (the whole backlog lives IN the ring, where it can be measured) and
 * excess beyond target+slack is SKIPPED — a one-shot jump-cut back toward
 * the live edge, applied by the interrupt so the read cursor has exactly one
 * writer.
 *
 * b53-b55, the deep-cushion experiment (design/CUSHION-PHYSICS.md has the
 * full post-mortem): with the decoder preemptive (b52) we tried holding
 * ~7.4 s of backlog as freeze immunity and jump-cutting to the live edge on
 * transport commands. Both cut triggers failed in the field — cut-on-send
 * (b53) jumps audibly when the command no-ops upstream (b37 dead context);
 * cut-on-confirmation (b54) misattributes NATURAL track flips that race a
 * pending command, stealing the very transition the listener was about to
 * hear (with a deep cushion, /now flips ~cushion seconds before the ear).
 * Underneath sits a conservation law: ear-lag == cushion — deep freeze
 * immunity and fast command response cannot coexist at one instant. b55
 * returns to the b31-proven shallow target; what the experiment KEEPS is the
 * preemptive decoder (instant catch-up, no decode-stop during freezes) and
 * the server-side queue-size (no more slow-client disconnects). */
#define CQ_AUD_PREBUF_PCM  (384L * 1024L)  /* ~2.2 s decoded before starting */
#define CQ_AUD_RING_TARGET (512L * 1024L)  /* trim back to ~2.9 s of backlog */
#define CQ_AUD_TRIM_SLACK  (256L * 1024L)  /* +1.5 s hysteresis before trimming */
#define CQ_AUD_COMP_MAX   (64 * 1024)      /* pre-play staging (header strip + sync) */
#define CQ_AUD_COMP_RING  (256L * 1024L)   /* compressed SPSC ring feeding the
                                              decode pipeline (cq_decring, b51) —
                                              ~16 s of 128 kbps ahead of the stage */

static cq_transport  *gPls    = NULL;   /* /spot/stream.pls discovery */
static cq_transport  *gStream = NULL;   /* the endless Icecast connection */
static au_state       gAuSt   = AU_IDLE;
static unsigned char  gComp[CQ_AUD_COMP_MAX];   /* pre-play staging: gStream drains
                                                   here through AU_HTTP/AU_SYNC; at
                                                   AU_PLAY the remainder hands off to
                                                   gDec and this buffer goes quiet */
static size_t         gCompLen = 0;
static Ptr            gCompRing = NULL;         /* comp SPSC ring, NewPtr'd with gRing */
static cq_decring     gDec;                     /* the decode pipeline (cq_decring.h):
                                                   b51 pumps it inline on the loop,
                                                   b52 moves the pump to an MP task */
static long           gDecResSeen = 0;          /* pump counters already narrated */
static long           gDecFluSeen = 0;
static cq_mp3_frame   gFmt;             /* from the first confirmed frame */
static SndChannelPtr  gChan = NULL;
static Ptr            gRing = NULL;               /* PCM ring, NewPtr'd on first listen */
static volatile unsigned long gRingWr = 0;        /* bytes written — event loop only */
static volatile unsigned long gRingRd = 0;        /* bytes read — interrupt only */
static SndDoubleBackUPP       gDBUPP = NULL;
static SndDoubleBufferHeader  gDBH;
static SndDoubleBufferPtr     gDB[2] = { NULL, NULL };
static int            gPlaying = 0;               /* SndPlayDoubleBuffer issued */
static volatile long  gDBFires = 0;               /* double-back invocations */
static volatile long  gDBSilence = 0;             /* refills served as silence */
static volatile long  gRingSkip = 0;              /* one-shot latency trim request:
                                                    loop sets it ONLY when zero,
                                                    interrupt applies + zeroes it —
                                                    gRingRd keeps a single writer */
static long           gTrims = 0;                 /* trims this listen */
static unsigned long  gStarveTick = 0;            /* tick the interrupt last
                                                     served silence; 0 = never
                                                     this listen (view input) */
static unsigned long  gRxTick = 0;                /* tick of the last stream
                                                     byte (view input; the old
                                                     gMountDry, now cq_view's) */
static unsigned long  gAuBeat = 0;                /* heartbeat log tick */
static long           gRxTotal = 0;               /* stream bytes received this listen */

/* Every transport that can be in flight at once (Fio E): poll, command,
 * cover, fire, search, queue and pls each ride their own connection, so one
 * client could hold 7 sockets on a fork-per-connection server. The automatic
 * starters (cover, queue poll) wait while at the budget; user-initiated
 * transactions are never gated. gStream deliberately does NOT count: the
 * budget protects gopher-spot, and the stream socket goes to Icecast. */
static int TxInFlight(void)
{
    return (gTx ? 1 : 0) + (gCmd ? 1 : 0) + (gCover ? 1 : 0) + (gFire ? 1 : 0) +
           (gSearchTx ? 1 : 0) + (gQueueTx ? 1 : 0) + (gPls ? 1 : 0) +
           (gFactTx ? 1 : 0);
}

static int ParseHttpUrl(const char *url, char *host, size_t hcap,
                        int *port, char *path, size_t pcap)
{
    const char *p, *slash, *colon;
    size_t hn, pn;

    *port = 80;
    if (strncmp(url, "http://", 7) != 0) return 0;
    p = url + 7;
    slash = strchr(p, '/');
    if (!slash) return 0;
    colon = strchr(p, ':');
    if (colon && colon < slash) { *port = atoi(colon + 1); hn = (size_t)(colon - p); }
    else                        { hn = (size_t)(slash - p); }
    pn = strlen(slash);
    if (hn == 0 || hn >= hcap || pn >= pcap) return 0;
    if (*port <= 0 || *port >= 65536) return 0;
    memcpy(host, p, hn); host[hn] = '\0';
    memcpy(path, slash, pn + 1);
    return 1;
}

/* Double-back proc — INTERRUPT TIME: refill one hardware buffer from the
 * ring. Only reads gRingRd/gRingWr and the ring bytes; a starved ring is
 * served as silence with the buffer still marked ready, so the channel
 * keeps running and playback resumes on its own. No Memory Manager, no
 * Toolbox, just copies. (The PCM producer behind gRingWr may become an MP
 * task in b52 — same SPSC contract, nothing changes on this side.) */
static pascal void AudioDoubleBack(SndChannelPtr chan, SndDoubleBufferPtr db)
{
    unsigned long avail;
    long want = CQ_AUD_CHUNK;
    (void)chan;
    gDBFires++;
    if (gRingSkip > 0) {           /* latency trim: jump-cut toward the live edge */
        unsigned long s = (unsigned long)gRingSkip;
        unsigned long have = gRingWr - gRingRd;
        if (s > have) s = have;
        gRingRd += s;
        gRingSkip = 0;
    }
    avail = gRingWr - gRingRd;
    /* Whole chunk or whole silence: padding partial trickles shredded the
     * audio into ~0.19 s confetti when production lagged (b25). Serving full
     * silence and leaving the partial to GROW turns a deficit into distinct
     * pauses instead of machine-gun stutter. (A dead stream's final partial
     * tail still plays — there is nothing left to wait for.) */
    long take = (avail >= (unsigned long)want) ? want
              : ((!gStream && avail > 0) ? (long)avail : 0);
    if (take > 0) {
        unsigned long rd = gRingRd % CQ_AUD_RING_BYTES;
        long first = (long)(CQ_AUD_RING_BYTES - rd);
        if (first > take) first = take;
        memcpy(db->dbSoundData, gRing + rd, (size_t)first);
        if (take > first)
            memcpy(db->dbSoundData + first, gRing, (size_t)(take - first));
        gRingRd += (unsigned long)take;
    }
    if (take < want) {
        memset(db->dbSoundData + take, 0, (size_t)(want - take));
        if (take == 0) gDBSilence++;
    }
    db->dbNumFrames = want / (2 * gFmt.channels);
    db->dbFlags |= dbBufferReady;
}

/* --- the decode task (b52, exp/mp-decode phase 2) ------------------------
 * The b50 probe proved QEMU/UTM OS 9 schedules MPLibrary tasks even while
 * the cooperative loop is frozen inside MenuSelect (beats +2552 across a
 * 15 s menu hold, VM logs 2026-07-06). So the pump moves off-loop: this
 * preemptive task runs cq_decring_pump — pure C, no Toolbox, no Memory
 * Manager, no OT — and the PCM ring keeps filling while menus, drags and
 * dialogs starve WaitNextEvent. Everything else stays on main: the wire
 * (OT is not MP-safe), the channel start (SndPlayDoubleBuffer is Toolbox),
 * the trim, all logging.
 *
 * Command protocol (single-writer, like every cursor here): main writes
 * gDecCmd; the task acks into gDecAck after completing a pass in that mode.
 * Cursor/decoder resets happen ONLY while the task acks PARK. gDecBeat is
 * the liveness counter the heartbeat narrates. */
enum { CQ_DEC_PARK = 0, CQ_DEC_RUN = 1, CQ_DEC_QUIT = 2 };

static int                    gMPOk      = 0;     /* MPLibraryIsLoaded() at boot */
static int                    gMPDecode  = 0;     /* pump on the task? chosen once
                                                     per listen; 0 = inline (b51) */
static MPTaskID               gDecTask   = NULL;
static MPQueueID              gDecQ      = NULL;  /* termination notifications */
static volatile long          gDecCmd    = CQ_DEC_PARK;   /* main writes */
static volatile long          gDecAck    = CQ_DEC_PARK;   /* task writes */
static volatile unsigned long gDecBeat   = 0;             /* task writes */
static unsigned long long     gAbsPerMs  = 1;    /* UpTime units per ms; main
                                                    converts gDec.dec_time */
static unsigned long long     gAbsPer5Ms = 0;    /* the task's nap, precomputed
                                                    on main so the task needs no
                                                    conversion calls */

static unsigned long long AbsToU64(AbsoluteTime t)
{
    return ((unsigned long long)t.hi << 32) | t.lo;
}

/* The pump's clock on the task: UpTime is MP-safe (TickCount is a trap —
 * illegal off the main context). */
static unsigned long long DecNowUp(void)
{
    return AbsToU64(UpTime());
}

static OSStatus DecodeTaskProc(void *param)
{
    cq_decring *r = (cq_decring *)param;
    for (;;) {
        long cmd = gDecCmd;
        if (cmd == CQ_DEC_QUIT) break;
        if (cmd == CQ_DEC_RUN) cq_decring_pump(r);
        gDecAck = cmd;            /* ack AFTER a full pass in that mode */
        gDecBeat++;
        {   /* ~5 ms nap: one 32 KB chunk lasts ~190 ms, so this is ~38x
             * service margin while staying polite to the blue task */
            unsigned long long w = DecNowUp() + gAbsPer5Ms;
            AbsoluteTime wake;
            wake.hi = (UInt32)(w >> 32);
            wake.lo = (UInt32)w;
            MPDelayUntil(&wake);
        }
    }
    gDecAck = CQ_DEC_QUIT;
    return noErr;                 /* task exit posts to gDecQ */
}

/* Create the task once, lazily, at the first listen; it stays up, PARKed
 * between listens (create/terminate per listen would just add races).
 * Any failure = inline fallback, i.e. exactly the b51 behavior. */
static void StartDecTask(void)
{
    OSStatus err;
    if (gDecTask || !gMPOk) return;
    err = MPCreateQueue(&gDecQ);
    if (err == noErr) {
        gDecCmd = gDecAck = CQ_DEC_PARK;
        err = MPCreateTask(DecodeTaskProc, &gDec, 128 * 1024, gDecQ,
                           NULL, NULL, 0, &gDecTask);
    }
    if (err != noErr) {
        DbgLog("audio: MPCreateTask failed (err=%ld) - inline decode", (long)err);
        if (gDecQ) { MPDeleteQueue(gDecQ); gDecQ = NULL; }
        gDecTask = NULL;
        return;
    }
    DbgLog("audio: decode task up (%ld cpu)", (long)MPProcessors());
}

/* Park the task and WAIT for the ack: after this returns, the task is
 * provably between passes and cursor/decoder resets can't race it. The
 * bound (~500 ms; a pass + nap is ~10 ms) only trips if the task died —
 * fall back inline and carry on. */
static void ParkDecTask(void)
{
    unsigned long t0;
    if (!gDecTask) return;
    gDecCmd = CQ_DEC_PARK;
    t0 = (unsigned long)TickCount();
    while (gDecAck != CQ_DEC_PARK) {
        if ((unsigned long)TickCount() - t0 > 30) {
            DbgLog("audio: park timeout - decode task dead? going inline");
            gMPDecode = 0;
            return;
        }
        MPYield();       /* preemptive anyway, but hand the slice over */
    }
}

/* App quit: park, ask the task to exit, join via the notify queue. */
static void QuitDecTask(void)
{
    void *p1, *p2, *p3;
    if (!gDecTask) return;
    ParkDecTask();
    gDecCmd = CQ_DEC_QUIT;
    if (MPWaitOnQueue(gDecQ, &p1, &p2, &p3,
                      1000 * kDurationMillisecond) != noErr) {
        DbgLog("audio: decode task join timeout, terminating");
        MPTerminateTask(gDecTask, noErr);
        MPWaitOnQueue(gDecQ, &p1, &p2, &p3, 1000 * kDurationMillisecond);
    }
    MPDeleteQueue(gDecQ);
    gDecQ = NULL;
    gDecTask = NULL;
    DbgLog("audio: decode task down, %lu beats", gDecBeat);
}

static void StopAudio(const char *why)
{
    if (gChan) { SndDisposeChannel(gChan, true); gChan = NULL; }   /* true = quiet NOW */
    if (gStream) { cq_tx_cancel(gStream); cq_tx_free(gStream); gStream = NULL; }
    if (gAuSt != AU_IDLE) DbgLog("audio: stopped (%s)", why);
    gAuSt = AU_IDLE;
    gCompLen = 0;
    ParkDecTask();             /* the pump may live on the MP task: cursor and
                                * decoder resets are legal only once it acks
                                * PARK (no task / task dead = plain inline) */
    cq_decring_reset(&gDec);
    gPlaying = 0;
    gRingWr = gRingRd = 0;
    gRingSkip = 0;
    gStarveTick = 0;
    gRxTick = 0;
}

/* First confirmed frame in hand: reset the decoder and arm the ring. The
 * decode step is minimp3 (cq_mp3dec) — QuickTime is NOT in this path; b13–b20
 * proved every QT route (URL movie, SoundConverter pull/push, both MP3
 * fourccs) consumes the stream and decodes nothing on QT 5.0.2. The channel
 * itself starts later, once the prebuffer is in the ring. */
/* The pump's clock (cq_decring's injected now hook). b51: the pump runs on
 * the event loop, so TickCount is legal and dec_time stays in ticks like the
 * old gDecTicks. b52 swaps in an UpTime wrapper for the MP task (TickCount is
 * a trap — illegal off the main context). */
static unsigned long long DecNowTicks(void)
{
    return (unsigned long long)TickCount();
}

static int OpenAudioOutput(void)
{
    if (!gRing) gRing = NewPtr(CQ_AUD_RING_BYTES);
    if (!gRing) { DbgLog("audio: NewPtr(ring %ld) failed", CQ_AUD_RING_BYTES); return 0; }
    if (!gCompRing) gCompRing = NewPtr(CQ_AUD_COMP_RING);
    if (!gCompRing) { DbgLog("audio: NewPtr(comp %ld) failed", CQ_AUD_COMP_RING); return 0; }
    StartDecTask();            /* lazy, once; failure leaves gDecTask NULL */
    ParkDecTask();             /* provably parked before the resets below
                                * (a fresh listen normally already is) */
    gMPDecode = (gDecTask != NULL);
    gRingWr = gRingRd = 0;
    cq_decring_init(&gDec, (unsigned char *)gCompRing, CQ_AUD_COMP_RING,
                    (unsigned char *)gRing, CQ_AUD_RING_BYTES,
                    &gRingWr, &gRingRd);
    gDec.now = gMPDecode ? DecNowUp : DecNowTicks;
    gDecResSeen = gDecFluSeen = 0;
    cq_mp3dec_init();
    gPlaying = 0;
    gDBFires = gDBSilence = 0;
    gRingSkip = 0;
    gTrims = 0;
    DbgLog("audio: output armed (minimp3), %d Hz %dch %d kbps",
           gFmt.samplerate, gFmt.channels, gFmt.bitrate_kbps);
    return 1;
}

/* Prebuffer reached: create the channel and start the double-buffer engine.
 * Both hardware buffers are pre-filled from the ring before the first note. */
static int StartDoubleBuffer(void)
{
    OSErr e;
    int b;

    if (!gDBUPP) gDBUPP = NewSndDoubleBackUPP(AudioDoubleBack);
    for (b = 0; b < 2; b++)
        if (!gDB[b])
            gDB[b] = (SndDoubleBufferPtr)NewPtrClear(sizeof(SndDoubleBuffer) + CQ_AUD_CHUNK);
    if (!gDBUPP || !gDB[0] || !gDB[1]) { DbgLog("audio: double-buffer alloc failed"); return 0; }

    e = SndNewChannel(&gChan, sampledSynth,
                      gFmt.channels == 1 ? initMono : initStereo, NULL);
    if (e != noErr) { DbgLog("audio: SndNewChannel err=%d", (int)e); gChan = NULL; return 0; }

    memset(&gDBH, 0, sizeof(gDBH));
    gDBH.dbhNumChannels   = (short)gFmt.channels;
    gDBH.dbhSampleSize    = 16;
    gDBH.dbhCompressionID = 0;
    gDBH.dbhPacketSize    = 0;
    gDBH.dbhSampleRate    = (UnsignedFixed)((unsigned long)gFmt.samplerate << 16);
    gDBH.dbhBufferPtr[0]  = gDB[0];
    gDBH.dbhBufferPtr[1]  = gDB[1];
    gDBH.dbhDoubleBack    = gDBUPP;
    gDB[0]->dbFlags = 0;
    gDB[1]->dbFlags = 0;
    AudioDoubleBack(gChan, gDB[0]);              /* pre-fill both buffers */
    AudioDoubleBack(gChan, gDB[1]);
    e = SndPlayDoubleBuffer(gChan, &gDBH);
    if (e != noErr) { DbgLog("audio: SndPlayDoubleBuffer err=%d", (int)e); return 0; }
    gPlaying = 1;
    DbgLog("audio: PLAY, ring %ld KB deep", (long)((gRingWr - gRingRd) / 1024));
    return 1;
}

/* The decode step itself — staging, the b26 tail-gate, the b27 resync, the
 * minimp3 call, the wrap-aware ring write — moved verbatim to the portable
 * pipeline in src/cq_decring.c (b51), where the host suite proves it
 * byte-identical to a flat decode. Here remains only the wire-side feeder:
 * drain the Icecast socket straight into the comp ring, zero-copy. */
static void DrainStreamToDec(void)
{
    for (;;) {
        unsigned long contig = 0;
        unsigned char *dst = cq_decring_claim(&gDec, &contig);
        size_t n;
        if (!contig) break;                    /* comp ring full: backpressure */
        n = cq_tx_drain(gStream, dst, (size_t)contig);
        if (!n) break;
        cq_decring_commit(&gDec, (unsigned long)n);
        gRxTotal += (long)n;
        /* The first ~12 KB, drain by drain: an impatience-proof rate read.
         * (VM tests keep getting toggled off within seconds — b16/b17.) */
        if (gRxTotal <= 12288)
            DbgLog("audio: rx +%ld (total %ld)", (long)n, gRxTotal);
    }
}

/* Advance the whole audio pipeline one bounded slice; never blocks. */
static void ServiceAudio(void)
{
    if (gAuSt == AU_IDLE) return;

    /* 1) the wire: poll, drain, note death. Pre-play the bytes stage in gComp
     * (header strip + sync need a flat window); once playing they feed the
     * comp ring, whose backpressure replaces the old staging-full check. */
    if (gStream) {
        cq_tx_status st = cq_tx_poll(gStream);
        if (gAuSt == AU_PLAY) {
            DrainStreamToDec();
        } else if (gCompLen < sizeof(gComp)) {
            size_t n = cq_tx_drain(gStream, gComp + gCompLen, sizeof(gComp) - gCompLen);
            gCompLen += n;
            gRxTotal += (long)n;
            /* The first ~12 KB, drain by drain: an impatience-proof rate read.
             * (VM tests keep getting toggled off within seconds — b16/b17.) */
            if (n && gRxTotal <= 12288)
                DbgLog("audio: rx +%ld (total %ld)", (long)n, gRxTotal);
        }
        if (st != CQ_TX_RUNNING) {
            DbgLog("audio: stream %s", st == CQ_TX_DONE ? "closed by server"
                                                        : cq_tx_error_message(gStream));
            if (gAuSt == AU_PLAY) {
                DrainStreamToDec();             /* last drain of the buffered tail */
                cq_tx_free(gStream); gStream = NULL;
                cq_decring_eof(&gDec);          /* let the pump flush the held
                                                   final frame (b26 gate 2 -> 1) */
            } else {
                if (gCompLen < sizeof(gComp))   /* last drain of the buffered tail */
                    gCompLen += cq_tx_drain(gStream, gComp + gCompLen, sizeof(gComp) - gCompLen);
                cq_tx_free(gStream); gStream = NULL;
                StopAudio("stream died before playback");
                return;
            }
        }
    }

    /* 2) strip the HTTP/ICY response header */
    if (gAuSt == AU_HTTP && gCompLen >= 4) {
        size_t i;
        for (i = 0; i + 3 < gCompLen; i++)
            if (gComp[i] == '\r' && gComp[i+1] == '\n' &&
                gComp[i+2] == '\r' && gComp[i+3] == '\n') break;
        if (i + 3 < gCompLen) {
            {   /* log the status line before dropping the header */
                char line[81];
                size_t k, n = 0;
                for (k = 0; k < gCompLen && n < 80 &&
                            gComp[k] != '\r' && gComp[k] != '\n'; k++)
                    line[n++] = (gComp[k] >= 32 && gComp[k] < 127) ? (char)gComp[k] : '.';
                line[n] = '\0';
                DbgLog("audio: response \"%s\" (+%ld header bytes)", line, (long)(i + 4));
            }
            gCompLen -= i + 4;
            memmove(gComp, gComp + i + 4, gCompLen);
            gAuSt = AU_SYNC;
        } else if (gCompLen > 8192) {
            StopAudio("no header end in 8 KB");
            return;
        }
    }

    /* 3) find the first confirmed MP3 frame, then open codec + channel */
    if (gAuSt == AU_SYNC) {
        long off = cq_mp3_sync(gComp, gCompLen, &gFmt);
        if (off >= 0) {
            DbgLog("audio: synced at +%ld: %d Hz %dch %d kbps, %d-byte frames",
                   off, gFmt.samplerate, gFmt.channels, gFmt.bitrate_kbps,
                   gFmt.frame_bytes);
            gCompLen -= (size_t)off;
            memmove(gComp, gComp + off, gCompLen);
            if (!OpenAudioOutput()) { StopAudio("output open failed"); return; }
            gAuSt = AU_PLAY;
            {   /* hand the synced remainder to the decode pipeline; the comp
                 * ring (256 KB) dwarfs this staging (64 KB), so it all fits */
                size_t done = 0;
                while (done < gCompLen) {
                    unsigned long contig = 0;
                    unsigned char *dst = cq_decring_claim(&gDec, &contig);
                    if (!contig) break;
                    if ((size_t)contig > gCompLen - done)
                        contig = (unsigned long)(gCompLen - done);
                    memcpy(dst, gComp + done, (size_t)contig);
                    cq_decring_commit(&gDec, contig);
                    done += (size_t)contig;
                }
                gCompLen = 0;
            }
            if (gMPDecode) {
                gDecCmd = CQ_DEC_RUN;   /* wake the task; from here the pump
                                           runs preemptively off-loop */
                DbgLog("audio: decode task RUNNING");
            }
        } else if (gCompLen > 32 * 1024) {
            StopAudio("no MP3 frame in 32 KB");
            return;
        }
    }

    /* 4) playback: pump the decode pipeline; drained-out = done. With the MP
     * task RUNNING the pump happens off-loop and main only reads counters;
     * inline fallback (no MPLibrary / task died) pumps right here (b51). */
    if (gAuSt == AU_PLAY) {
        if (!gMPDecode) cq_decring_pump(&gDec);
        {   /* narrate the pump's mailbox (the module may not DbgLog — it has
             * to stay legal on an MP task) */
            if (gDec.resyncs != gDecResSeen) {
                gDecResSeen = gDec.resyncs;
                DbgLog("audio: resync x%ld (%ld B splice junk dropped)",
                       gDec.resyncs, gDec.resync_bytes);
            }
            if (gDec.flushes != gDecFluSeen) {
                gDecFluSeen = gDec.flushes;
                DbgLog("audio: staging unparseable, flushed (x%ld)", gDec.flushes);
            }
        }
        if (!gPlaying) {
            unsigned long fill = gRingWr - gRingRd;
            /* start at the prebuffer mark — or with whatever remains if the
             * stream already died (play the tail out rather than sit on it).
             * SndPlayDoubleBuffer is Toolbox: this decision stays on main. */
            if (fill >= (unsigned long)CQ_AUD_PREBUF_PCM || (!gStream && fill > 0)) {
                if (!StartDoubleBuffer()) StopAudio("output start failed");
            }
        }
        if (gAuSt != AU_PLAY) return;   /* the start may give up and stop */
        {   /* status-tell timestamps (b45): cq_view turns these into the
             * "buffering..." starve window and the dry-mount heuristic —
             * here we only note WHEN silence was served / rx last moved. */
            static long prevSil = 0;
            static long prevRx  = -1;
            unsigned long now = (unsigned long)TickCount();
            if (gDBSilence != prevSil) {
                /* counters reset per listen while these statics survive:
                 * only a real INCREMENT is a starve, a drop is a new listen */
                if (gDBSilence > prevSil) gStarveTick = now;
                prevSil = gDBSilence;
            }
            if (gRxTotal != prevRx) {
                prevRx = gRxTotal;
                gRxTick = now;
            }
        }
        /* Latency trim (b31): backlog beyond target+slack means we fell
         * behind the live edge (menu freeze / dry-spell catch-up). Ask the
         * interrupt to skip the oldest PCM — one jump-cut back to ~target. */
        if (gPlaying && gRingSkip == 0) {
            unsigned long fill = gRingWr - gRingRd;
            if (fill > (unsigned long)(CQ_AUD_RING_TARGET + CQ_AUD_TRIM_SLACK)) {
                gRingSkip = (long)(fill - CQ_AUD_RING_TARGET);
                gTrims++;
                DbgLog("audio: latency trim, skipping %ld KB of backlog",
                       (long)((fill - CQ_AUD_RING_TARGET) / 1024));
            }
        }
        if (!gStream && cq_decring_idle(&gDec) && gPlaying && gRingWr == gRingRd) {
            StopAudio("played out");
            return;
        }
        if ((unsigned long)TickCount() - gAuBeat >= (gPlaying ? 600 : 300)) {
            gAuBeat = (unsigned long)TickCount();
            /* rx total is THE starvation tell: it should grow ~16000 B/s at
             * 128 kbps; a parked rx total = the mount is dry (Spotify paused /
             * playing off-device), not a client bug (b16 log). silence>0 with
             * ring>0 would be the impossible case — the interrupt starving
             * while the loop has data. */
            if (!gPlaying)
                DbgLog("audio: prebuffering ring %ld/%ld KB, %ld rx",
                       (long)((gRingWr - gRingRd) / 1024),
                       (long)(CQ_AUD_PREBUF_PCM / 1024), gRxTotal);
            else {
                long uok = 0, ufail = 0, uerr = 0;
                /* dec_time unit differs per context (ticks inline, UpTime on
                 * the task) — normalize to ms so builds stay comparable */
                long decms = gMPDecode
                           ? (long)(gDec.dec_time / gAbsPerMs)
                           : (long)(gDec.dec_time * 50 / 3);
                cq_tx_udp_stats(&uok, &ufail, &uerr);
                DbgLog("audio: playing, ring %ld KB, fires %ld, sil %ld, trims %ld, dec %ldf/%ldms, pcm %ldK, %ld staged, %ld rx, mp %lu, udp %ld/%ld e%ld",
                       (long)((gRingWr - gRingRd) / 1024),
                       gDBFires, gDBSilence, gTrims, gDec.frames,
                       decms, gDec.pcm_total / 1024,
                       (long)(gDec.comp_wr - gDec.comp_rd + gDec.stage_len),
                       gRxTotal, (unsigned long)gDecBeat, uok, ufail, uerr);
                {   /* the task-alive tell: RUNNING but no beats since the
                     * last heartbeat = the decoder is gone; say so loudly */
                    static unsigned long prevBeat = 0;
                    if (gMPDecode && gDecCmd == CQ_DEC_RUN &&
                        gDecBeat == prevBeat)
                        DbgLog("audio: MP TASK STALLED (beat %lu)", gDecBeat);
                    prevBeat = gDecBeat;
                }
            }
        }
    } else if ((unsigned long)TickCount() - gAuBeat >= 300) {   /* pre-play: ~5 s */
        gAuBeat = (unsigned long)TickCount();
        DbgLog("audio: %s, %ld staged, %ld rx",
               gAuSt == AU_HTTP ? "awaiting header" : "syncing",
               (long)gCompLen, gRxTotal);
    }
}

/* --- the view (fio B): gather → render → diff → draw ---------------------
 * Every word the player area shows is cq_view's verdict over ONE input
 * struct built here — the b45/b48/b49 vocabulary moved into the pure module
 * where the host suite proves it; this glue only ferries state across. */
static void BuildViewIn(cq_view_in *in)
{
    unsigned long now = (unsigned long)TickCount();
    memset(in, 0, sizeof(*in));
    in->snap       = gHaveSnap ? &gSnap : NULL;
    in->snap_ticks = gSnapTick;

    in->has_stream_fact = (gStreamProbe == 1 && gHaveFact);
    in->live            = gFactLive;
    in->fact_ts         = gFactTs;

    in->engine = gAuSt == AU_IDLE                     ? CQ_ENGINE_OFF :
                 (gAuSt == AU_HTTP || gAuSt == AU_SYNC) ? CQ_ENGINE_TUNING :
                 !gPlaying                            ? CQ_ENGINE_BUFFERING :
                                                        CQ_ENGINE_ON_AIR;
    in->ring_fill     = gRingWr - gRingRd;
    in->prebuf_target = (unsigned long)CQ_AUD_PREBUF_PCM;
    in->chunk_bytes   = (unsigned long)CQ_AUD_CHUNK;
    in->rx_dry_ticks  = (gStream && gRxTick) ? (long)(now - gRxTick) : -1;
    in->starve_ticks  = gStarveTick ? (long)(now - gStarveTick) : -1;

    in->intent       = gIntent.kind;
    in->intent_ticks = gIntent.ticks;
    memcpy(in->pre_track_id, gIntent.pre_track_id, sizeof(in->pre_track_id));
    in->pre_state    = gIntent.pre_state;

    in->now_ticks = now;
}

static void RenderView(void)
{
    cq_view_in in;
    BuildViewIn(&in);
    cq_view_render(&gView, &in);
    if (gIntent.kind != CQ_INTENT_NONE && !gView.ack_active)
        gIntent.kind = CQ_INTENT_NONE;   /* reflected — or settle timed out */
}

/* Each loop pass: render, diff against the DRAWN truth, repaint only on a
 * display-significant change (≤2 Hz — the b45 animator's ceiling, now
 * change-driven: the buffering count-up and the second hand still tick). */
static void TickView(void)
{
    static unsigned long lastDraw = 0;
    unsigned long now = (unsigned long)TickCount();
    cq_view_in in;
    cq_view    v;
    if (now - lastDraw < 30) return;
    BuildViewIn(&in);
    cq_view_render(&v, &in);
    if (gIntent.kind != CQ_INTENT_NONE && !v.ack_active)
        gIntent.kind = CQ_INTENT_NONE;
    if (cq_view_differs(&gView, &v)) {
        lastDraw = now;
        Redraw();                        /* re-renders into gView, then paints */
    }
}

/* ⌘T target: open the Icecast mount discovered in the PLS. */
static void StartStream(const char *url)
{
    char host[64], path[160], sel[192];
    int  port;

    if (!ParseHttpUrl(url, host, sizeof(host), &port, path, sizeof(path))) {
        DbgLog("audio: unparseable url \"%s\"", url);
        return;
    }
    /* selector + the transport's closing CRLF = a complete HTTP/1.0 request
     * (the blank line) — the exact wire the b15 probe validated. */
    snprintf(sel, sizeof(sel), "GET %s HTTP/1.0\r\n", path);
    gStream = cq_tx_stream_new(host, port, sel);
    if (!gStream) { DbgLog("audio: stream tx alloc failed"); return; }
    cq_tx_start(gStream);
    gAuSt = AU_HTTP;
    gAuBeat = (unsigned long)TickCount();
    gRxTotal = 0;
    DbgLog("audio: streaming GET %s:%d%s", host, port, path);
}

/* Listen / Stop toggle — ⌘T and the AppleScript "listen"/"stop" commands;
 * the button retired in b46 (auto-start made it ceremonial), and the status
 * readout (b45) is the visible state. */
static void ToggleListen(void)
{
    if (gAuSt != AU_IDLE) { StopAudio("user toggle"); return; }
    if (gPls) return;
    gPls = cq_tx_new(gHost, gPort, "/spot/stream.pls");   /* discover the stream URL */
    if (gPls) {
        cq_tx_start(gPls);
        DbgLog("audio: fetching /spot/stream.pls from %s:%d", gHost, gPort);
    } else {
        DbgLog("audio: pls tx alloc failed");
    }
}

/* Advance the search/queue/fire transactions; parse a list reply and fill it. */
static void PumpAux(void)
{
    if (gFire && cq_tx_poll(gFire) != CQ_TX_RUNNING) {
        cq_tx_free(gFire); gFire = NULL;
        /* A command (play / queue-add) just landed. The server is eventually
         * consistent (~1-2 s), so schedule ONE refetch ~2 s out (Fio H,
         * CLIENTS.md g13) instead of refetching instantly and usually
         * missing the change. */
        gQueueKick = (unsigned long)TickCount() + 120;
    }

    ServiceAudio();                                       /* non-blocking audio service */

    if (gPls) {
        cq_tx_status st = cq_tx_poll(gPls);
        if (st == CQ_TX_DONE) {
            size_t len = 0;
            const unsigned char *d = cq_tx_data(gPls, &len);
            char *body = (char *)malloc(len + 1);
            DbgLog("audio: pls reply %ld bytes", (long)len);
            if (body) {
                char *url;
                memcpy(body, d, len); body[len] = '\0';
                url = cq_pls_first_url(body);             /* PLS/M3U -> first stream URL */
                free(body);
                if (url) { StartStream(url); free(url); }
                else DbgLog("audio: no url found in pls body");
            } else {
                DbgLog("audio: pls body malloc(%ld) failed", (long)len + 1);
            }
            cq_tx_free(gPls); gPls = NULL;
        } else if (st == CQ_TX_FAILED) {
            DbgLog("audio: pls fetch FAILED e%d %s",
                   (int)cq_tx_error_code(gPls), cq_tx_error_message(gPls));
            cq_tx_free(gPls); gPls = NULL;
        }
    }

    if (gSearchTx) {
        cq_tx_status st = cq_tx_poll(gSearchTx);
        if (st == CQ_TX_DONE) {
            size_t len = 0;
            const unsigned char *d = cq_tx_data(gSearchTx, &len);
            /* One reply carries all three kinds (b57): tracks (item.*), artists
             * (artist.*) and albums (album.*). Parse each; FillResults lays them
             * out artist/album/track. */
            cq_track_list_free(&gSearchItems);
            cq_ref_list_free(&gSearchArtists);
            cq_ref_list_free(&gSearchAlbums);
            cq_track_list_from_response(&gSearchItems, d, len);
            cq_ref_list_from_response(&gSearchArtists, d, len, "artist");
            cq_ref_list_from_response(&gSearchAlbums, d, len, "album");
            gAlbumBrowse = 0;
            FillResults();
            cq_tx_free(gSearchTx); gSearchTx = NULL;
        } else if (st == CQ_TX_FAILED) { cq_tx_free(gSearchTx); gSearchTx = NULL; }
    }
    if (gArtistTx) {
        cq_tx_status st = cq_tx_poll(gArtistTx);
        if (st == CQ_TX_DONE) {
            size_t len = 0;
            const unsigned char *d = cq_tx_data(gArtistTx, &len);
            /* The discography rows are item.<i>.{id,name} — same shape as a ref
             * block, prefix "item". Flip the list into browse mode. */
            cq_ref_list_free(&gArtistAlbums);
            cq_ref_list_from_response(&gArtistAlbums, d, len, "item");
            gAlbumBrowse = 1;
            FillResults();
            cq_tx_free(gArtistTx); gArtistTx = NULL;
        } else if (st == CQ_TX_FAILED) { cq_tx_free(gArtistTx); gArtistTx = NULL; }
    }
    if (gQueueTx) {
        cq_tx_status st = cq_tx_poll(gQueueTx);
        if (st == CQ_TX_DONE) {
            size_t len = 0;
            const unsigned char *d = cq_tx_data(gQueueTx, &len);
            cq_track_list fresh;
            cq_track_list_from_response(&fresh, d, len);
            /* Law-2 for the queue (b33): Spotify reads as EMPTY during idle /
             * track changes / rate-limit — with the list always visible, one
             * spurious empty used to wipe "Up Next" in your face every few
             * polls. Keep the last non-empty snapshot; adopt empty only when
             * it repeats (a truly emptied queue settles within ~3 polls). */
            if (fresh.count > 0) {
                cq_track_list_free(&gQueueItems);
                gQueueItems = fresh;
                FillList(gQueueList, &gQueueItems);
                gQueueEmptyRuns = 0;
                if (gAutoPlayPending) {
                    /* launch found the device stopped (dead context); now
                     * that the queue is visible, play its head (b47) */
                    gAutoPlayPending = 0;
                    if (gSnap.state != CQ_STATE_PLAYING) {
                        DbgLog("auto: play queue head on launch (%ld queued)",
                               (long)gQueueItems.count);
                        SetIntent(CQ_INTENT_PLAY);
                        PlayFrom(&gQueueItems, 0, NULL);
                    }
                }
            } else {
                cq_track_list_free(&fresh);
                if (++gQueueEmptyRuns == 2 && gQueueItems.count > 0) {
                    /* two in a row (b39; was three = a 30 s stale window) */
                    cq_track_list_free(&gQueueItems);
                    FillList(gQueueList, &gQueueItems);   /* now genuinely empty */
                }
            }
            cq_tx_free(gQueueTx); gQueueTx = NULL;
            gQueueLast = (unsigned long)TickCount();
            cq_backoff_ok(&gQBackoff);
        } else if (st == CQ_TX_FAILED) {
            cq_tx_free(gQueueTx); gQueueTx = NULL;
            gQueueLast = (unsigned long)TickCount();
            cq_backoff_fail(&gQBackoff);   /* failing? re-fetch slower (Fio D) */
        }
    } else if (!gSuspended && TxInFlight() < CQ_MAX_INFLIGHT) {
        /* Keep the always-visible queue list live (~10 s, backing off on
         * errors): the queue changes underneath us — adds from search, tracks
         * being consumed, and Spotify's "idle player reads as empty" quirk all
         * resolve on the next poll. The kick (Fio H) is the one-shot
         * post-command refetch that makes adds feel immediate. */
        unsigned long now = (unsigned long)TickCount();
        int due = now - gQueueLast >= (unsigned long)cq_backoff_interval(&gQBackoff);
        if (gQueueKick && (long)(now - gQueueKick) >= 0) due = 1;
        if (due) {
            gQueueKick = 0;
            gQueueTx = cq_tx_new(gHost, gPort, "/spot/api/1/queue");
            if (gQueueTx) cq_tx_start(gQueueTx);
        }
    }

    /* The media plane's fact (fio B): /spot/api/1/stream at the lazy 10 s
     * cadence (CLIENTS.md rule 23 — it reconciles, it doesn't render).
     * Feature-detected once per launch (rule 24): not_found = old server,
     * stop asking; cq_view then keeps the rx-dry heuristic for the run. */
    if (gFactTx) {
        cq_tx_status st = cq_tx_poll(gFactTx);
        if (st == CQ_TX_DONE) {
            size_t len = 0;
            const unsigned char *d = cq_tx_data(gFactTx, &len);
            cq_fields f;
            const char *err;
            cq_fields_init(&f);
            cq_fields_parse(&f, d, len);
            err = cq_fields_get(&f, "error");
            if (err && strcmp(err, "not_found") == 0) {
                gStreamProbe = -1;
                DbgLog("stream: not_found - old server, rx heuristic this run");
            } else if (err) {
                /* upstream (Icecast unreachable) etc: keep the last fact —
                 * cq_view ages it out against the advancing /now ts */
                cq_backoff_fail(&gFBackoff);
            } else {
                int live = (int)cq_parse_ll(cq_fields_get(&f, "live"));
                gStreamProbe = 1;
                if (!gHaveFact || live != gFactLive)
                    DbgLog("stream: live %d, %ld listeners", live,
                           (long)cq_parse_ll(cq_fields_get(&f, "listeners")));
                gFactLive = live;
                gFactTs   = cq_parse_ll(cq_fields_get(&f, "ts"));
                gHaveFact = 1;
                cq_backoff_ok(&gFBackoff);
            }
            cq_fields_free(&f);
            cq_tx_free(gFactTx); gFactTx = NULL;
            gFactLast = (unsigned long)TickCount();
        } else if (st == CQ_TX_FAILED) {
            cq_tx_free(gFactTx); gFactTx = NULL;
            gFactLast = (unsigned long)TickCount();
            cq_backoff_fail(&gFBackoff);
        }
    } else if (!gSuspended && gStreamProbe >= 0 &&
               TxInFlight() < CQ_MAX_INFLIGHT) {
        unsigned long now = (unsigned long)TickCount();
        if (now - gFactLast >= (unsigned long)cq_backoff_interval(&gFBackoff) ||
            gFactLast == 0) {
            gFactLast = now;
            gFactTx = cq_tx_new(gHost, gPort, "/spot/api/1/stream");
            if (gFactTx) cq_tx_start(gFactTx);
        }
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
    /* The DLOG resource is `invisible` (built hidden so we can populate the edit
     * fields before it paints). It MUST be shown before ModalDialog — otherwise
     * the modal loop runs on a hidden dialog whose Save/Cancel buttons can't be
     * clicked, so it never sees item 1/2 and never returns: the whole machine
     * appears frozen (cursor moves, nothing responds). */
    ShowWindow(d);

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
        cq_backoff_init(&gQBackoff, CQ_QUEUE_POLL_TICKS, CQ_POLL_CAP);
        cq_backoff_init(&gFBackoff, CQ_STREAM_POLL_TICKS, CQ_POLL_CAP);
        gLastPoll = 0;
        gStreamProbe = 0;                              /* new host: re-probe /stream */
        gHaveFact = 0;
        gFactLast = 0;
        while (cq_cache_count(&gCovers) > 0) {          /* refetch covers from the new host */
            GWorldPtr gw = (GWorldPtr)cq_cache_take_oldest(&gCovers);
            if (gw) DisposeGWorld(gw);
        }
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

/* --- Apple Events (b36): the smoke-test surface ---------------------------
 * Required suite (oapp/odoc/pdoc no-ops, quit) plus misc/dosc ("do script"),
 * whose direct object is one command string. From Script Editor on the VM:
 *     tell application "Casquinha" to «event miscdosc» "listen"
 * Commands: listen stop play pause next prev wake add search:<query>
 * Every event is logged, so a scripted run narrates itself in the log. */
static void DoScriptCmd(const char *cmd)
{
    if      (strcmp(cmd, "listen") == 0) { if (gAuSt == AU_IDLE) ToggleListen(); }
    else if (strcmp(cmd, "stop")   == 0) { if (gAuSt != AU_IDLE) ToggleListen(); }
    else if (strcmp(cmd, "play")   == 0) { SetIntent(CQ_INTENT_PLAY);
                                           StartCommand("/spot/api/1/play"); }
    else if (strcmp(cmd, "pause")  == 0) { SetIntent(CQ_INTENT_PAUSE);
                                           StartCommand("/spot/api/1/pause"); }
    else if (strcmp(cmd, "next")   == 0) NextCommand();
    else if (strcmp(cmd, "prev")   == 0) { SetIntent(CQ_INTENT_SKIP);
                                           StartCommand("/spot/api/1/prev"); }
    else if (strcmp(cmd, "wake")   == 0) { SetIntent(CQ_INTENT_PLAY);
                                           StartCommand("/spot/api/1/wake?play=1"); }
    else if (strncmp(cmd, "search:", 7) == 0) {
        if (gSearchEdit) {
            SetControlData(gSearchEdit, kControlNoPart, kControlEditTextTextTag,
                           (Size)strlen(cmd + 7), (Ptr)(cmd + 7));
            Draw1Control(gSearchEdit);
            RunSearch();
        }
    }
    else if (strcmp(cmd, "add") == 0) {    /* enqueue the selected (or first) result */
        Cell c;
        SetPt(&c, 0, 0);
        if (gSearchList && !LGetSelect(true, &c, gSearchList) &&
            gSearchItems.count > 0) {
            SetPt(&c, 0, 0);
            LSetSelect(true, c, gSearchList);
        }
        QueueSelected();
    }
    else DbgLog("apple-event: unknown command \"%s\"", cmd);
}

static pascal OSErr AENoopHandler(const AppleEvent *evt, AppleEvent *reply, long refCon)
{
    (void)evt; (void)reply; (void)refCon;
    return noErr;
}

static pascal OSErr AEQuitHandler(const AppleEvent *evt, AppleEvent *reply, long refCon)
{
    (void)evt; (void)reply; (void)refCon;
    DbgLog("apple-event: quit");
    gRunning = false;
    return noErr;
}

static pascal OSErr AEDoScriptHandler(const AppleEvent *evt, AppleEvent *reply, long refCon)
{
    char     buf[256];
    Size     got = 0;
    DescType typ;
    OSErr    e;
    (void)reply; (void)refCon;
    e = AEGetParamPtr(evt, keyDirectObject, typeChar, &typ,
                      buf, sizeof(buf) - 1, &got);
    if (e != noErr) return e;
    if (got < 0) got = 0;
    buf[got] = '\0';
    DbgLog("apple-event: do script \"%s\"", buf);
    DoScriptCmd(buf);
    return noErr;
}

static void InstallAEHandlers(void)
{
    AEInstallEventHandler(kCoreEventClass, kAEOpenApplication,
                          NewAEEventHandlerUPP(AENoopHandler), 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEOpenDocuments,
                          NewAEEventHandlerUPP(AENoopHandler), 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEPrintDocuments,
                          NewAEEventHandlerUPP(AENoopHandler), 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEQuitApplication,
                          NewAEEventHandlerUPP(AEQuitHandler), 0, false);
    AEInstallEventHandler(kAEMiscStandards, kAEDoScript,
                          NewAEEventHandlerUPP(AEDoScriptHandler), 0, false);
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
            /* b30: search/queue/listen/wake are main-window controls now —
             * nothing you'd touch mid-listening is left behind a menu. */
            if (item == kPrefsItem)      DoPrefs();
            else if (item == kQuitItem)  gRunning = false;
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
            if (TrackGoAway(win, ev->where) && win == gWindow)
                gRunning = false;
            break;
        case inContent:
            if (win != FrontWindow()) SelectWindow(win);
            else if (win == gWindow)  DoContentClick(win, ev->where);
            break;
    }
}

/* --- the cooperative loop, decomposed (b50) ------------------------------
 * main()'s while stays the only clock; these are its limbs, extracted
 * verbatim so the loop body reads as the schedule it actually is. */

/* Short sleep while a fetch is in flight (spin the OT state machine),
 * calmer when idle between polls, calmer still suspended in the
 * background — unless audio is playing, which wants regular service. */
static long ComputeSleep(void)
{
    return gTx ? 1L : ((gSuspended && gAuSt == AU_IDLE) ? 60L : 10L);
}

static void HandleEvent(EventRecord *ev)
{
    switch (ev->what) {
        case mouseDown:
            DoMouseDown(ev);
            break;
        case keyDown:
        case autoKey: {
            char ch = (char)(ev->message & charCodeMask);
            if (ev->modifiers & cmdKey) {
                /* Hand-rolled shortcuts (b30): these actions left the
                 * menu (menu tracking starves the audio ring) but keep
                 * their keys. Everything else -> MenuKey (Prefs/Quit). */
                if (ch == 'f' || ch == 'F') {
                    if (gSearchEdit)
                        SetKeyboardFocus(gWindow, gSearchEdit,
                                         kControlFocusNextPart);
                } else if (ch == 't' || ch == 'T') {
                    ToggleListen();
                } else if (ch == 'k' || ch == 'K') {
                    SetIntent(CQ_INTENT_PLAY);
                    StartCommand("/spot/api/1/wake?play=1");
                } else if (ch == 'u' || ch == 'U') {
                    gQueueKick = (unsigned long)TickCount();  /* refresh now */
                } else {
                    DoMenu(MenuKey(ch));
                }
            } else {
                ControlHandle focus = NULL;
                GetKeyboardFocus(gWindow, &focus);
                if (focus && focus == gSearchEdit) {
                    if (ch == '\r' || ch == '\n') RunSearch();
                    else {
                        /* Same port trap as IdleControls: HandleControlKey
                         * draws the typed character into the CURRENT
                         * port — pin it to the window. */
                        GrafPtr save;
                        GetPort(&save);
                        SetPort((GrafPtr)gWindow);
                        HandleControlKey(gSearchEdit,
                             (short)((ev->message & keyCodeMask) >> 8), ch, ev->modifiers);
                        SetPort(save);
                    }
                }
            }
            break;
        }
        case updateEvt: {
            WindowRef win = (WindowRef)ev->message;
            BeginUpdate(win);
            if (win == gWindow) {
                DrawWindowContents(win);          /* the player area */
                DrawShelf(win);                   /* separator/labels/frames */
                UpdateControls(win, ((GrafPtr)win)->visRgn);
                if (gSearchList) LUpdate(((GrafPtr)win)->visRgn, gSearchList);
                if (gQueueList)  LUpdate(((GrafPtr)win)->visRgn, gQueueList);
            }
            EndUpdate(win);
            break;
        }
        case osEvt:
            /* Suspend/resume (Fio C; SIZE has acceptSuspendResumeEvents):
             * in the background no NEW polls start (PollNetwork/PumpAux
             * check gSuspended) — in-flight transactions still drain and
             * ⌘T audio plays on. Resume kicks an immediate /now. */
            if (((ev->message >> 24) & 0xFF) == suspendResumeMessage) {
                gSuspended = (ev->message & resumeFlag) == 0;
                if (!gSuspended) gLastPoll = 0;
            }
            break;
        case kHighLevelEvent:
            AEProcessAppleEvent(ev);   /* quit + do script (b36) */
            break;
        default:
            break;
    }
}

static void TickCaret(void)
{
    GrafPtr save;
    if (!gWindow) return;
    /* IdleControls draws into the CURRENT port — pin it to the window
     * or the caret lands wherever the last draw left the port. */
    GetPort(&save);
    SetPort((GrafPtr)gWindow);
    IdleControls(gWindow);
    SetPort(save);
}

int main(void)
{
    EventRecord ev;
    GDHandle    gd;

    /* minimp3 keeps ~15 KB of scratch on the STACK per decoded frame; carve
     * out headroom before anything runs deep (stack grows down into the gap
     * this opens above the heap), then let the heap take the rest. */
    SetApplLimit((Ptr)((unsigned long)GetApplLimit() - 64 * 1024));
    MaxApplZone();

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();

    RegisterAppearanceClient();               /* opt into the Platinum look */

    gQTOk = (EnterMovies() == noErr);         /* QuickTime for cover-art decode */
    gMPOk = MPLibraryIsLoaded();              /* preemptive tasks available? (b50) */
    if (gMPOk) {
        /* UpTime<->wall units, computed once on main: the decode task must
         * not call conversion services, it just adds raw u64s (b52) */
        gAbsPerMs  = AbsToU64(DurationToAbsolute(kDurationMillisecond));
        gAbsPer5Ms = AbsToU64(DurationToAbsolute(5 * kDurationMillisecond));
        if (!gAbsPerMs) gAbsPerMs = 1;
    }
    {   /* boot line: which build, and exactly which QuickTime/MP is under us */
        long qtv = 0;
        Gestalt(gestaltQuickTimeVersion, &qtv);
        DbgLog("boot %s  QT=%s ver=%08lx  MP=%s", CQ_BUILD_TAG,
               gQTOk ? "ok" : "MISSING", qtv, gMPOk ? "yes" : "no");
    }
    cq_tx_set_log(TxProbeLog);   /* arm the b60 OT-trap freeze probe */

    {   /* converters: UTF-8 -> MacRoman (draw) and MacRoman -> UTF-8 (wire) */
        TextEncoding utf8 = CreateTextEncoding(kTextEncodingUnicodeV2_0,
                                kTextEncodingDefaultVariant, kUnicodeUTF8Format);
        if (TECCreateConverter(&gConv, utf8, kTextEncodingMacRoman) != noErr)
            gConv = NULL;
        if (TECCreateConverter(&gConvOut, kTextEncodingMacRoman, utf8) != noErr)
            gConvOut = NULL;
    }

    gd = GetMainDevice();                      /* depth/color for theme text */
    if (gd) {
        gDepth   = (**(**gd).gdPMap).pixelSize;
        gIsColor = TestDeviceAttribute(gd, gdDevType);
    }

    cq_guard_init(&gGuard);
    cq_backoff_init(&gBackoff, CQ_POLL_TICKS, CQ_POLL_CAP);
    cq_backoff_init(&gQBackoff, CQ_QUEUE_POLL_TICKS, CQ_POLL_CAP);
    cq_backoff_init(&gFBackoff, CQ_STREAM_POLL_TICKS, CQ_POLL_CAP);
    cq_cache_init(&gCovers);
    cq_debounce_init(&gDeb);
    LoadPrefs();                              /* server address from the prefs file */

    {   /* hold OPTION at launch to start quiet (no auto listen/wake, b43) */
        KeyMap km;
        GetKeys(km);
        if ((((unsigned char *)km)[0x3A >> 3] >> (0x3A & 7)) & 1) {
            gAutoStart = 0;
            DbgLog("auto: skipped (option held at launch)");
        }
    }
    memset(&gSnap, 0, sizeof(gSnap));

    InstallAEHandlers();                       /* scriptability (b36) */
    SetUpMenus();

    gWindow = GetNewWindow(kMainWindow, NULL, (WindowPtr)-1);
    if (gWindow) {
        SetThemeWindowBackground(gWindow, kThemeBrushDialogBackgroundActive, false);
        SetPort((GrafPtr)gWindow);
        MakeControls(gWindow);
        ShowWindow(gWindow);
    }

    if (gAutoStart && gAuSt == AU_IDLE) {
        /* Tune in right away (b43). A dry mount is fine — the engine waits
         * in its prebuffering state and starts by itself once the launch
         * wake (fired off the first /now snapshot) opens the tap. */
        DbgLog("auto: listen on launch");
        ToggleListen();
    }

    while (gRunning) {
        if (WaitNextEvent(everyEvent, &ev, ComputeSleep(), NULL))
            HandleEvent(&ev);
        PollNetwork();       /* /now poll + command/cover/debounce pumps */
        PumpAux();           /* fire/pls/search/queue/stream pumps + ServiceAudio */
        TickCaret();         /* blink the search caret (port-pinned) */
        TickView();          /* render → diff → repaint on change (≤2 Hz) */
    }

    StopAudio("quit");       /* parks the decode task before cursor resets */
    QuitDecTask();
    if (gLogRef) { DbgLog("quit"); FSClose(gLogRef); gLogRef = 0; }
    if (gTx) { cq_tx_cancel(gTx); cq_tx_free(gTx); }
    cq_now_free(&gSnap);
    FlushEvents(everyEvent, -1);
    return 0;
}
