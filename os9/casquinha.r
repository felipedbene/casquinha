/*
 * casquinha.r — resources for the Mac OS 9 app (Fio 3).
 * The 'cfrg' + 'SIZE' are inlined (not RetroPPCAPPL.r) because Fio C needs a
 * custom SIZE: acceptSuspendResumeEvents so the event loop can pause polling
 * in the background, and canBackground so an in-flight transaction still
 * drains (instead of stalling with the server holding the socket) and the
 * ⌘T audio stream keeps playing while the app is behind another.
 */
#include "Processes.r"
#include "CodeFragments.r"
#include "Windows.r"
#include "Menus.r"
#include "Dialogs.r"

resource 'cfrg' (0) {
    {
        kPowerPCCFragArch, kIsCompleteCFrag, kNoVersionNum, kNoVersionNum,
        kDefaultStackSize, kNoAppSubFolder,
        kApplicationCFrag, kDataForkCFragLocator, kZeroOffset, kCFragGoesToEOF,
        "Casquinha"
    }
};

resource 'SIZE' (-1) {
    reserved,
    acceptSuspendResumeEvents,   /* Fio C: osEvt drives the poll pause */
    reserved,
    canBackground,               /* drain in-flight tx + service audio behind */
    doesActivateOnFGSwitch,
    backgroundAndForeground,
    dontGetFrontClicks,
    ignoreChildDiedEvents,
    is32BitCompatible,
    notHighLevelEventAware,
    onlyLocalHLEvents,
    notStationeryAware,
    dontUseTextEditServices,
    reserved,
    reserved,
    reserved,
    1536 * 1024,                 /* preferred: room for the 8-slot cover cache (Fio A) */
    1024 * 1024                  /* minimum: unchanged from the toolchain default */
};

/* ---- Main document window ---- */
resource 'WIND' (128, "Casquinha") {
    { 48, 60, 284, 520 },        /* top, left, bottom, right (236 x 460) */
    documentProc,
    visible,
    goAway,
    0x0,
    "Casquinha",
    noAutoCenter
};

/* ---- Menu bar: Apple, File ---- */
resource 'MBAR' (128) {
    { 128, 129 }
};

resource 'MENU' (128) {
    128, textMenuProc,
    0x7FFFFFFD,                  /* title + item 1 enabled */
    enabled, apple,
    {
        "About Casquinha...", noicon, nokey, nomark, plain
    }
};

resource 'MENU' (129, "File") {
    129, textMenuProc, allEnabled, enabled, "File",
    {
        "Search...",      noicon, "F", nomark, plain;
        "Queue",          noicon, "U", nomark, plain;
        "Listen / Stop",  noicon, "T", nomark, plain;
        "-",              noicon, nokey, nomark, plain;
        "Preferences...", noicon, ",", nomark, plain;
        "-",              noicon, nokey, nomark, plain;
        "Quit",           noicon, "Q", nomark, plain
    }
};

/* ---- Preferences dialog (Fio 6): server host + port ---- */
resource 'DLOG' (129, "Prefs") {
    { 90, 120, 250, 470 },
    movableDBoxProc, invisible, noGoAway, 0x0, 129, "gopher-spot Server",
    noAutoCenter
};

resource 'DITL' (129) {
    {
        { 128, 260, 148, 330 }, Button { enabled, "Save" };
        { 128, 168, 148, 238 }, Button { enabled, "Cancel" };
        {  46,  80,  62, 330 }, EditText { enabled, "" };          /* 3: host */
        {  46,  16,  62,  76 }, StaticText { disabled, "Host:" };  /* 4 */
        {  76,  80,  92, 180 }, EditText { enabled, "" };          /* 5: port */
        {  76,  16,  92,  76 }, StaticText { disabled, "Port:" };  /* 6 */
        {  12,  16,  40, 330 }, StaticText { disabled,
            "Address of the gopher-spot server (LAN)." };          /* 7 */
    }
};

/* ---- About box ---- */
resource 'ALRT' (128) {
    { 60, 60, 200, 400 },
    128,
    {
        OK, visible, sound1;
        OK, visible, sound1;
        OK, visible, sound1;
        OK, visible, sound1
    },
    alertPositionMainScreen
};

resource 'DITL' (128) {
    {
        { 108, 260, 128, 320 },
        Button { enabled, "OK" };

        { 12, 20, 96, 328 },
        StaticText { disabled,
            "Casquinha - a gopher-spot Spotify remote for Mac OS 9 (PowerPC)."
            "  The essential Radinho, on the oldest Mac in the house." };
    }
};
