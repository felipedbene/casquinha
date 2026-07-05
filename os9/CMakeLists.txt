cmake_minimum_required(VERSION 3.10)
project(Casquinha C)

# add_application() comes from the Retro68 classic-PPC toolchain file. It
# compiles the sources, runs Rez on the .r, and emits the Classic-Mac outputs
# (.bin / .dsk / APPL). The app links the REAL pure core from ../src.
add_application(Casquinha
    casquinha.c
    casquinha.r
    ../src/cq_codec.c
    ../src/cq_now.c
    ../src/cq_track.c
    ../src/cq_guard.c
    ../src/cq_debounce.c
)

target_include_directories(Casquinha PRIVATE ../src)

# Smallest possible app: drop unused functions.
set_target_properties(Casquinha PROPERTIES COMPILE_OPTIONS -ffunction-sections)
set_target_properties(Casquinha PROPERTIES LINK_FLAGS "-Wl,-gc-sections")
