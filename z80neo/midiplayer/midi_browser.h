/*
 * midi_browser.h - shared file browser + replay front end for the z80neo MIDI
 * players (bare-metal +z80neo and CP/M MIDI.COM).
 *
 * Both players drive the same firmware file API and the same replay loop
 * (midi_delay.asm); only the console primitives differ, so the console is
 * abstracted behind mb_con_putc()/mb_con_get(), which each front end provides.
 * A '*' prefix on a browser entry loops one file; .mid/.midi filtering applies
 * to the play-all modes.  The optional playback dashboard (midi_ui.c) is
 * compiled in when MIDI_UI is defined on the command line (the CP/M build).
 */
#ifndef MIDI_BROWSER_H
#define MIDI_BROWSER_H

#include <stdint.h>

/* Firmware file API on ports 0xA0-0xA3 (see firmware/z80neo/include/fileio.h). */
#define FCMD_CLOSE     2
#define FCMD_DIRRESET  5
#define FCMD_DIRNEXT   6
#define FCMD_OPEN_MIDI 8
#define FCMD_REMOUNT_CPM 9
#define FCMD_DIRPUSH   11
#define FCMD_DIRPOP    12

/* Command status bits (read back from 0xA0). */
#define FST_EOF  0x02
#define FST_ERR  0x04
#define FST_OPEN 0x08
#define FST_NAME 0x10
#define FST_DIR  0x40

#define MIDI_BAUD_CODE 10   /* 31250 in the firmware table */
#define MIDI_FMT_8N1   0x03 /* data bits 8, stop 1, parity none */
#define MIDI_DIR       "/MIDI"

#define MB_NAME_MAX   128
#define MB_PAGE_LINES 20

/* Replay loop + calibrated wait (midi_delay.asm). */
extern void midi_delay_ms(uint16_t ms);
extern void midi_play(void);
/* Set by midi_play: 0 = end of song, 1 = 'q'/ESC, 2 = 'n' (next track). */
extern uint8_t midi_stopped;

/* Console primitives provided by each front end. */
void mb_con_putc(uint8_t c);
int mb_con_get(void);

/* Console helpers built on mb_con_putc(). */
void mb_puts(const char *s);
void mb_num(uint16_t v);
void mb_hex8(uint8_t v);

/* Hand the SD volume back to CP/M (command 9); a no-op call for the native
 * player, which never needs it. */
void mb_remount_cpm(void);

/* Entropy counter advanced while the browser blocks on a key. */
extern uint16_t mb_wait_ticks;

/* Current directory ("" = card root); persisted across songs. */
extern char mb_dir[MB_NAME_MAX];

/* Set by mb_browse() to 1 when the chosen file should repeat. */
extern uint8_t mb_act_loop;

/* Browser result codes. */
#define MB_ACT_QUIT   0
#define MB_ACT_FILE   1
#define MB_ACT_ALL    2
#define MB_ACT_ALLR   3
#define MB_ACT_RAND   4
#define MB_ACT_RANDR  5

/* Play-mode flags for mb_play_dir(). */
#define MB_PLAY_RAND   0x01
#define MB_PLAY_REPEAT 0x02

uint8_t mb_pick_start_dir(void);
uint8_t mb_browse(char *out);
void mb_make_path(char *out, uint8_t max, const char *name);
void mb_play_file(const char *path, uint8_t loop);
void mb_play_dir(uint8_t flags);

#endif
