cmake_minimum_required(VERSION 3.10)
project(Casquinha C)

# add_application() comes from the Retro68 classic-PPC toolchain file. It
# compiles the sources, runs Rez on the .r, and emits the Classic-Mac outputs
# (.bin / .dsk / APPL). The app links the REAL pure core from ../src.
add_application(Casquinha
    CREATOR Casq                 # matches the signature in icon.r; enables the icon
    casquinha.c
    casquinha.r
    icon.r                       # BNDL/FREF/signature + the otter icon family
    ../src/cq_codec.c
    ../src/cq_now.c
    ../src/cq_track.c
    ../src/cq_guard.c
    ../src/cq_view.c              # the one renderable truth (fio B)
    ../src/cq_debounce.c
    ../src/cq_backoff.c
    ../src/cq_cache.c
    ../src/cq_pls.c
    ../src/cq_mp3.c               # MP3 frame sync for the ⌘T audio wire (b16)
    ../src/cq_mp3dec.c            # minimp3 decode seam (b21 — QT decodes nothing)
    ../src/cq_decring.c           # comp SPSC ring + decode pump (b51, exp/mp-decode)
    ../src/cq_transport_ot.c      # Open Transport backend (guarded by CQ_OS9)
)

target_include_directories(Casquinha PRIVATE ../src)
target_compile_definitions(Casquinha PRIVATE CQ_OS9)

# Open Transport: application-context init (InitOpenTransport) + core endpoint
# calls + the Internet module (OTOpenInternetServices / OTInetStringToAddress /
# OTInitInetAddress). Order matters for static resolution (glue before imports).
target_link_libraries(Casquinha
    OpenTransportAppPPC
    OpenTptInetPPC
    OpenTptInternetLib
    OpenTransportLib
    AppearanceLib          # Platinum theme drawing (DrawThemeTrack, theme colors)
    TextEncodingConverter  # UTF-8 -> MacRoman at the draw boundary
    TextCommon
    QuickTimeLib           # GraphicsImporter cover-art decode (Fio 5)
    SoundLib               # SndChannel + SoundConverter (⌘T audio, b16)
    MPLibrary              # preemptive tasks (exp/mp-decode, b50 smoke probe)
    DriverServicesLib      # UpTime/AddDurationToAbsolute for MP task pacing
)

# The MP3 decoder must OUTRUN realtime on an emulated PPC (38 frames/s at
# 44.1 kHz); b25 showed it lagging. Full throttle for that one TU: -O3 and
# fast-math (minimp3 is float-heavy, and QEMU's PPC FP helpers are pricey).
set_source_files_properties(../src/cq_mp3dec.c PROPERTIES
    COMPILE_OPTIONS "-O3;-ffast-math;-fno-math-errno")

# Smallest possible app: drop unused functions.
set_target_properties(Casquinha PROPERTIES COMPILE_OPTIONS -ffunction-sections)
set_target_properties(Casquinha PROPERTIES LINK_FLAGS "-Wl,-gc-sections")
