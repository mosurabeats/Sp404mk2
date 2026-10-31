/*
 * Everything that makes the OS look and feel like its owner lives here:
 * the name on the boot screen, the words it uses, the track names you
 * pick from, and the pad colours. Change this file to restyle the OS;
 * nothing else needs to move.
 */
#ifndef MOS_THEME_H
#define MOS_THEME_H

#define MOS_THEME_NAME     "MOSURA OS"
#define MOS_THEME_VERSION  "0.1"
#define MOS_THEME_TAGLINE  "MPC FLOW / SP SOUL"
#define MOS_THEME_BOOT_MS  1500

/* Track names you pick from on the TRACK screen (8 characters max). */
static const char *const mos_theme_track_names[] = {
    "KICK", "SNARE", "HATS", "PERC", "CLAP", "RIM", "SHAKER", "TOMS",
    "BASS", "KEYS", "CHORDS", "LEAD", "PAD", "CHOP", "LOOP", "VOX",
    "BREAK", "SAMPLE", "STAB", "SWEEP", "FX", "NOISE", "TRACK",
};
#define MOS_THEME_TRACK_NAME_COUNT ((int)(sizeof mos_theme_track_names / sizeof mos_theme_track_names[0]))

/* Default names of tracks 1-16, as indexes into the list above. */
static const int mos_theme_default_tracks[16] = { 0, 1, 2, 3, 8, 9, 13, 15, 22, 22, 22, 22, 22, 22, 22, 22 };

/* Screen colours for the simulator's screenshots (the SP's own OLED has one colour). */
#define MOS_THEME_SCREEN_ON  0xE6F0FF
#define MOS_THEME_SCREEN_OFF 0x07080C

/* Pad LED colours, 0xRRGGBB. */
#define MOS_COLOR_IDLE      0x000000 /* pad with nothing on it */
#define MOS_COLOR_LOADED    0x1A0A2E /* pad with a sample: dim violet */
#define MOS_COLOR_HIT       0xB04CFF /* pad being played: bright violet */
#define MOS_COLOR_SYNTH     0x0A2A2A /* chromatic pad on a synth track: dim teal */
#define MOS_COLOR_ROOT      0x1F6F6F /* the C of each octave on a synth track */
#define MOS_COLOR_TRACK_ON  0x2FD07F /* TRACK screen: playing track */
#define MOS_COLOR_TRACK_OFF 0x40100A /* TRACK screen: muted track */
#define MOS_COLOR_SELECTED  0xFFFFFF /* TRACK screen: the active track */
#define MOS_COLOR_SOLO      0xFFB000 /* TRACK screen: soloed track */
#define MOS_COLOR_STEP      0xFF3D7F /* STEP screen: pads that have an event at the cursor */

#endif
