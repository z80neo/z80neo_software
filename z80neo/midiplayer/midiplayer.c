/*
 * midiplayer.c - z80neo SD-card Standard MIDI File player
 *
 * Bare-metal +z80neo program (linked at 0x0000).  Plays a .mid out the second
 * UART the firmware exposes on ports 0x90 (data) / 0x91 (status) at the MIDI
 * rate, 31250 baud 8N1.
 *
 * The Pico parses the SMF and merges its tracks; the Z80 only has to wait and
 * emit.  The file API's FILE_OPEN_MIDI command (0xA0) turns port 0xA2 into a
 * stream of grouped records (see firmware/z80neo/include/midi.h):
 *
 *     [0] delay lo   } milliseconds before this record
 *     [1] delay hi   }
 *     [2] byte count 0 = delay only, 0xFF = end, else raw bytes follow
 *     [3..]          byte count raw MIDI message bytes, forwarded as-is
 *
 * Every channel message sharing a tick is coalesced into one record, so the
 * Z80 waits once per tick and then blits the tick's bytes, instead of paying
 * the per-record delay/console cost on every event.  This replaced an on-Z80
 * SMF parser: at 128 kHz the C parser cost 100-230 ms per event (~15x slow),
 * while streaming pre-parsed records costs only the port accesses.  The Pico
 * has the speed to do the parsing.
 *
 * The browser, directory walk and replay front end are shared with the CP/M
 * player in midi_browser.c; this file only provides the console primitives and
 * the UART1 setup.  There is no playback UI.
 *
 * Build:  make            -> build/midiplayer.ihx
 * Deploy: copy to the SD root and point Z80NEO.INI PROG1 at it.
 */

#include <stdint.h>

#include "midi_browser.h"

/* ---- Ports ------------------------------------------------------------- */

__sfr __at 0x80 CON_DATA;
__sfr __at 0x81 CON_STATUS; /* bit0 = RX ready, bit1 = TX ready */
__sfr __at 0x92 MIDI_CFG_BAUD;
__sfr __at 0x93 MIDI_CFG_FMT;

/* Console primitives (the shared browser calls these). */
void mb_con_putc(uint8_t c) {
	while ((CON_STATUS & 0x02) == 0)
		;
	CON_DATA = c;
}

int mb_con_get(void) {
	if ((CON_STATUS & 0x01) == 0)
		return 0;
	return (int)(CON_DATA & 0x7F);
}

/* Put UART1 at the MIDI rate: format first (with the current baud), then the
 * MIDI baud code, so the final configure uses 31250 8N1. */
static void midi_init(void) {
	MIDI_CFG_FMT = MIDI_FMT_8N1;
	MIDI_CFG_BAUD = MIDI_BAUD_CODE;
}

int main(void) {
	char base[MB_NAME_MAX];
	char path[MB_NAME_MAX + 16];
	uint8_t act, flags;

	mb_puts("\r\nz80neo MIDI player\r\n");
	midi_init();
	mb_puts("UART1 -> 31250 8N1 (MIDI); cfg code ");
	mb_num((uint16_t)MIDI_CFG_BAUD);
	mb_puts(MIDI_CFG_BAUD == MIDI_BAUD_CODE ? " ok"
											: "  (need firmware code 10)");
	mb_puts("\r\n\r\n");

	/* Pick a start directory, then browse and play in a loop, so the current
	 * directory persists across songs (no reset needed to walk the tree). */
	if (!mb_pick_start_dir()) {
		mb_puts("No files found in ");
		mb_puts(MIDI_DIR);
		mb_puts(" or the root\r\n");
		return 1;
	}

	for (;;) {
		act = mb_browse(base);
		if (act == MB_ACT_QUIT)
			break;
		if (act == MB_ACT_FILE) {
			mb_make_path(path, sizeof path, base);
			mb_play_file(path, mb_act_loop);
		} else {
			flags = 0;
			if (act == MB_ACT_RAND || act == MB_ACT_RANDR)
				flags |= MB_PLAY_RAND;
			if (act == MB_ACT_ALLR || act == MB_ACT_RANDR)
				flags |= MB_PLAY_REPEAT;
			mb_play_dir(flags);
		}
	}

	mb_puts("Bye.\r\n");
	return 0;
}
