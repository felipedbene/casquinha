/*
 * casquinha.r — resources for the Mac OS 9 app (Fio 3).
 * RetroPPCAPPL.r supplies the standard 'cfrg' + 'SIZE'; we add the window,
 * the menu bar, and the About box.
 */
#include "RetroPPCAPPL.r"
#include "Windows.r"
#include "Menus.r"
#include "Dialogs.r"

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
        "Quit", noicon, "Q", nomark, plain
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
