/*
 * midi_ui.c - Z80-native MIDI playback dashboard.
 *
 * Draws the dashboard with the VDP "ESC g" bitmap protocol on the console port
 * 0x80 (640x256, 16 colours); the NullVDP folds the palette to 8 colours.  The
 * firmware is not involved: it only streams grouped records.  The replay loop
 * calls midi_ui_record() once per record with the record's raw bytes in
 * midi_ui_buf[], so this file owns all MIDI parsing for the UI and redraws only
 * the fields that change.
 *
 * Coordinates are pixels in the bitmap layer; text uses font8.h (8x8).
 */
#include "midi_ui.h"

#include "font8.h"

#define CH 16

#define TITLE_Y 4
#define SONG_Y 16
#define INFO_Y 30
#define RULE_Y 42
#define HEAD_Y 46
#define ROW_Y 60
#define ROW_H 12

#define CH_X 8
#define INST_X 32
#define NOTE_X 136
#define VEL_X 176
#define BAR_X 208
#define BAR_W 88
#define VOL_X 304
#define PAN_X 344
#define PAN_W 88
#define TIME_X 48

#define SONG_MAX 76

uint8_t midi_ui_buf[MIDI_UI_BUF_MAX];
uint8_t midi_ui_n;
uint16_t midi_ui_ms;

static const char *const families[16] = {
	"Piano",	"Chromatic", "Organ",	   "Guitar", "Bass",	   "Strings",
	"Ensemble", "Brass",	 "Reed",	   "Pipe",	 "Synth Lead", "Synth Pad",
	"Synth FX", "Ethnic",	 "Percussive", "SFX"};

static const char *const note_names[12] = {"C",	 "C#", "D",	 "D#", "E",	 "F",
										   "F#", "G",  "G#", "A",  "A#", "B"};

/* Drawn values (0xFF = unknown / not yet drawn). */
static uint8_t sh_active[CH], sh_note[CH], sh_vel[CH], sh_prog[CH], sh_vol[CH];
static uint16_t sh_pan[CH];
static uint8_t d_active[CH], d_note[CH], d_vel[CH], d_prog[CH], d_vol[CH];
static uint8_t d_pan[CH];
static uint16_t dirty;

static uint8_t run_stat;
static uint8_t pend_st, pend_len, pend_have, pend_d1, pend_d2;

static uint32_t elapsed_ms;
static uint16_t shown_sec;

static uint8_t cur_fg = 0xFF;

/* ---- Console / ESC g ---------------------------------------------------- */

#ifndef MIDI_UI_NO_SFR
__sfr __at 0x80 MIDI_UI_CON;
__sfr __at 0x81 MIDI_UI_ST;

void midi_ui_putc(uint8_t c) {
	while ((MIDI_UI_ST & 0x02) == 0)
		;
	MIDI_UI_CON = c;
}
#endif

static void gcmd(uint8_t c) {
	midi_ui_putc(27);
	midi_ui_putc('g');
	midi_ui_putc(c);
}

static void gword(uint16_t v) {
	midi_ui_putc((uint8_t)v);
	midi_ui_putc((uint8_t)(v >> 8));
}

static void gcolor(uint8_t c) {
	if (c != cur_fg) {
		gcmd(3);
		midi_ui_putc(c);
		midi_ui_putc(0);
		cur_fg = c;
	}
}

static void gfill(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
	gcmd(6);
	gword(x);
	gword(y);
	gword(w);
	gword(h);
	midi_ui_putc(1);
}

static void gmode(uint8_t on) {
	gcmd(1);
	midi_ui_putc(on);
}

/* ---- Text --------------------------------------------------------------- */

static void draw_char(uint16_t x, uint16_t y, char c, uint8_t col) {
	const uint8_t *g;
	uint8_t r;

	if (c >= 'a' && c <= 'z')
		c = (char)(c - 32);
	if (c >= FONT8_FIRST && c <= FONT8_LAST)
		g = font8_ascii[c - FONT8_FIRST];
	else
		g = font8_ascii[0];

	gcolor(0);
	gfill(x, y, 8, 8);
	gcolor(col);
	for (r = 0; r < 8; r++) {
		uint8_t bits = g[r];
		uint8_t i = 0;
		while (i < 8) {
			if (bits & (0x80u >> i)) {
				uint8_t w = 1;
				while (i + w < 8 && (bits & (0x80u >> (i + w))))
					w++;
				gfill(x + i, y + r, w, 1);
				i = (uint8_t)(i + w);
			} else {
				i++;
			}
		}
	}
}

static void draw_text(uint16_t x, uint16_t y, const char *s, uint8_t len,
					  uint8_t col) {
	uint8_t i;
	for (i = 0; i < len; i++) {
		char c = s[i];
		if (c == '\0')
			c = ' ';
		draw_char(x + (uint16_t)i * 8, y, c, col);
	}
}

/* Right-aligned 3-digit field ("  0".."127"). */
static void fmt3(char *b, uint8_t v) {
	b[0] = ' ';
	b[1] = ' ';
	b[2] = ' ';
	b[3] = '\0';
	if (v >= 100)
		b[0] = (char)('0' + v / 100);
	if (v >= 10)
		b[1] = (char)('0' + (v / 10) % 10);
	b[2] = (char)('0' + v % 10);
}

static void fmt_note(char *b, uint8_t n) {
	int oct = (int)(n / 12) - 1;
	uint8_t k = 0;

	b[k++] = note_names[n % 12][0];
	if (note_names[n % 12][1])
		b[k++] = note_names[n % 12][1];
	if (oct < 0) {
		b[k++] = '-';
		b[k++] = (char)('0' - oct);
	} else {
		b[k++] = (char)('0' + oct);
	}
	while (k < 4)
		b[k++] = ' ';
	b[4] = '\0';
}

static uint8_t ch_color(uint8_t c) {
	static const uint8_t col[4] = {9, 10, 12, 14};
	return col[c & 3];
}

/* ---- Time --------------------------------------------------------------- */

static void draw_time(void) {
	char b[6];
	uint16_t sec = (uint16_t)(elapsed_ms / 1000U);
	uint16_t m = (uint16_t)(sec / 60U);
	uint16_t s = (uint16_t)(sec % 60U);

	if (m > 99) {
		m = 99;
		s = 59;
	}
	b[0] = (char)('0' + m / 10);
	b[1] = (char)('0' + m % 10);
	b[2] = ':';
	b[3] = (char)('0' + s / 10);
	b[4] = (char)('0' + s % 10);
	b[5] = '\0';
	draw_text(TIME_X, INFO_Y, b, 5, 15);
	shown_sec = sec;
}

/* ---- Channel rows ------------------------------------------------------- */

static void draw_channel(uint8_t c) {
	uint16_t y = (uint16_t)(ROW_Y + (uint16_t)c * ROW_H);
	uint8_t col = ch_color(c);
	uint8_t first = (sh_active[c] == 0xFF);
	uint8_t act_chg = (uint8_t)(first || sh_active[c] != d_active[c]);
	char b[13];

	if (first) {
		b[0] = (char)('0' + (c + 1) / 10);
		b[1] = (char)('0' + (c + 1) % 10);
		draw_text(CH_X, y, b, 2, col);
	}

	if (first || sh_prog[c] != d_prog[c]) {
		const char *fam = "---";
		if (d_prog[c] != 0xFF)
			fam = families[(d_prog[c] >> 3) & 0x0F];
		draw_text(INST_X, y, fam, 12, 7);
		sh_prog[c] = d_prog[c];
	}

	if (first || act_chg || sh_note[c] != d_note[c]) {
		if (d_active[c])
			fmt_note(b, d_note[c]);
		else {
			b[0] = '-';
			b[1] = '-';
			b[2] = '-';
			b[3] = '-';
			b[4] = '\0';
		}
		draw_text(NOTE_X, y, b, 4, 15);
	}

	if (first || act_chg || sh_vel[c] != d_vel[c]) {
		uint8_t v = d_active[c] ? d_vel[c] : 0;
		fmt3(b, v);
		draw_text(VEL_X, y, b, 3, 7);
		gcolor(8);
		gfill(BAR_X, y, BAR_W, 6);
		if (v) {
			gcolor(col);
			gfill(BAR_X, y, (uint16_t)(1U + ((uint16_t)v * (BAR_W - 1)) / 127U),
				  6);
		}
	}

	if (first || sh_vol[c] != d_vol[c]) {
		fmt3(b, d_vol[c]);
		draw_text(VOL_X, y, b, 3, 7);
		sh_vol[c] = d_vol[c];
	}

	if (first || sh_pan[c] != d_pan[c]) {
		gcolor(8);
		gfill(PAN_X, y, PAN_W, 6);
		gcolor(7);
		gfill(PAN_X + PAN_W / 2, y, 1, 6);
		gcolor(col);
		gfill(PAN_X + ((uint16_t)d_pan[c] * (PAN_W - 3)) / 127U, y, 3, 6);
		sh_pan[c] = d_pan[c];
	}

	sh_note[c] = d_note[c];
	sh_vel[c] = d_vel[c];
	sh_active[c] = d_active[c];
}

static void flush_channels(void) {
	uint8_t c, budget = 2;

	for (c = 0; c < CH && budget; c++) {
		if (dirty & (uint16_t)(1u << c)) {
			dirty &= (uint16_t)~(1u << c);
			draw_channel(c);
			budget--;
		}
	}
}

/* ---- MIDI parse --------------------------------------------------------- */

static void apply(uint8_t st, uint8_t d1, uint8_t d2) {
	uint8_t c = st & 0x0F;
	uint8_t type = st & 0xF0;

	switch (type) {
	case 0x90: /* note on (velocity 0 = note off) */
		if (d2 != 0) {
			d_active[c] = 1;
			d_note[c] = d1 & 0x7F;
			d_vel[c] = d2 & 0x7F;
			dirty |= (uint16_t)(1u << c);
		} else if (d_active[c] && d_note[c] == (d1 & 0x7F)) {
			d_active[c] = 0;
			dirty |= (uint16_t)(1u << c);
		}
		break;
	case 0x80: /* note off */
		if (d_active[c] && d_note[c] == (d1 & 0x7F)) {
			d_active[c] = 0;
			dirty |= (uint16_t)(1u << c);
		}
		break;
	case 0xC0: /* program change */
		if (d_prog[c] != (d1 & 0x7F)) {
			d_prog[c] = d1 & 0x7F;
			dirty |= (uint16_t)(1u << c);
		}
		break;
	case 0xB0: /* control change */
		if (d1 == 7) {
			if (d_vol[c] != (d2 & 0x7F)) {
				d_vol[c] = d2 & 0x7F;
				dirty |= (uint16_t)(1u << c);
			}
		} else if (d1 == 10) {
			if (d_pan[c] != (d2 & 0x7F)) {
				d_pan[c] = d2 & 0x7F;
				dirty |= (uint16_t)(1u << c);
			}
		} else if (d1 == 120 || d1 == 123) {
			if (d_active[c]) {
				d_active[c] = 0;
				dirty |= (uint16_t)(1u << c);
			}
		}
		break;
	default:
		break;
	}
}

void midi_ui_record(void) {
	uint8_t i;

	elapsed_ms += midi_ui_ms;

	for (i = 0; i < midi_ui_n; i++) {
		uint8_t b = midi_ui_buf[i];
		if (b & 0x80) {
			if (b >= 0x80 && b <= 0xEF) {
				run_stat = b;
				pend_st = b;
				pend_len = (uint8_t)(((b & 0xE0) == 0xC0) ? 1 : 2);
				pend_have = 0;
			} else {
				run_stat = 0; /* system message: drop running status */
			}
			continue;
		}
		if (run_stat == 0)
			continue;
		if (pend_have == 0)
			pend_d1 = b;
		else
			pend_d2 = b;
		pend_have++;
		if (pend_have >= pend_len) {
			apply(pend_st, pend_d1, pend_d2);
			pend_have = 0;
		}
	}

	if ((uint16_t)(elapsed_ms / 1000U) != shown_sec)
		draw_time();
	flush_channels();
}

/* ---- Start / stop ------------------------------------------------------- */

void midi_ui_start(const char *song) {
	uint8_t i;

	run_stat = 0;
	pend_have = 0;
	pend_len = 0;
	dirty = 0;
	elapsed_ms = 0;
	shown_sec = 0xFFFF;
	cur_fg = 0xFF;

	for (i = 0; i < CH; i++) {
		d_active[i] = 0;
		d_note[i] = 0;
		d_vel[i] = 0;
		d_prog[i] = 0xFF;
		d_vol[i] = 100;
		d_pan[i] = 64;
		sh_active[i] = 0xFF;
		sh_note[i] = 0xFF;
		sh_vel[i] = 0xFF;
		sh_prog[i] = 0xFF;
		sh_vol[i] = 0xFF;
		sh_pan[i] = 0xFFFF;
	}

	gmode(1); /* bitmap on (clears to black) */

	draw_text(8, TITLE_Y, "Z80NEO MIDI PLAYER", 18, 15);
	draw_text(8, SONG_Y, song, SONG_MAX, 10);
	draw_text(8, INFO_Y, "TIME", 4, 7);
	draw_time();
	draw_text(520, INFO_Y, "Q:STOP N:NEXT", 13, 7);

	gcolor(9);
	gfill(8, RULE_Y, 624, 1);

	draw_text(CH_X, HEAD_Y, "CH", 2, 7);
	draw_text(INST_X, HEAD_Y, "INSTRUMENT", 10, 7);
	draw_text(NOTE_X, HEAD_Y, "NOTE", 4, 7);
	draw_text(VEL_X, HEAD_Y, "VEL", 3, 7);
	draw_text(BAR_X, HEAD_Y, "LEVEL", 5, 7);
	draw_text(VOL_X, HEAD_Y, "VOL", 3, 7);
	draw_text(PAN_X, HEAD_Y, "PAN", 3, 7);
}

void midi_ui_stop(void) {
	gmode(0); /* back to the text console / CCP prompt */
	cur_fg = 0xFF;
}
