/*
 * midi_ui.h - Z80-native MIDI playback dashboard (used by MIDI.COM)
 *
 * The dashboard is drawn entirely by the Z80 with the VDP "ESC g" bitmap
 * protocol on the console port 0x80; the firmware only streams records.  It
 * lives here but is only linked by the CP/M player (`software/cpm/midiplayer`),
 * which assembles `midi_delay.asm` with -DMIDI_UI to enable the replay-loop
 * hook.  The native +z80neo player draws no UI during play.
 *
 * The replay loop (midi_delay.asm) copies every raw MIDI byte of a record into
 * midi_ui_buf[] and calls midi_ui_record() once per record.  The UI parses the
 * channel messages to keep a per-channel table, then redraws only the changed
 * fields, so its cost stays a small, bounded part of the (already idle) wait
 * between records.
 */
#ifndef MIDI_UI_H
#define MIDI_UI_H

#include <stdint.h>

/* Largest record the firmware emits (midi.h MIDI_GROUP_MAX). */
#define MIDI_UI_BUF_MAX 255

/* Raw bytes of the current record, filled by midi_delay.asm before each call
 * to midi_ui_record(), and its length (also set by the replay loop). */
extern uint8_t midi_ui_buf[MIDI_UI_BUF_MAX];
extern uint8_t midi_ui_n;
/* Delay the replay loop is about to wait (ms), for the elapsed-time clock. */
extern uint16_t midi_ui_ms;

/* Draw the static layout (bitmap on, title, song name, column headers) and
 * reset the per-channel state for a new song. */
void midi_ui_start(const char *song);

/* Parse midi_ui_buf[0..midi_ui_n-1], update the channel table, advance the
 * elapsed clock by midi_ui_ms and redraw a few changed fields. */
void midi_ui_record(void);

/* Switch the VDP back to text mode (the file browser / CCP prompt). */
void midi_ui_stop(void);

/* One console byte out port 0x80, waiting for TX-ready on 0x81. */
void midi_ui_putc(uint8_t c);

#endif
