/*
 * midi_browser.c - shared file browser + replay front end for the z80neo MIDI
 * players.  See midi_browser.h.
 *
 * The player starts in /MIDI (falling back to the card root) and walks the card
 * as a tree: directories are listed with a trailing '/', a number opens a file
 * or descends, ".." goes up, n/p page, a/A play all (once/loop), r/R play
 * randomly (once/loop), and *<num>/ *<name> plays one entry on repeat.  Play-all
 * only opens .mid/.midi files; a file chosen by name is opened as given.
 */
#include <stdint.h>

#include "midi_browser.h"

#ifdef MIDI_UI
#include "midi_ui.h"
#endif

__sfr __at 0xA0 FILE_CMD;
__sfr __at 0xA1 FILE_NAME;
__sfr __at 0xA2 FILE_DATA;
__sfr __at 0xA3 FILE_RST;

uint16_t mb_wait_ticks;
char mb_dir[MB_NAME_MAX];
uint8_t mb_act_loop;

/* ---- Console helpers ---------------------------------------------------- */

void mb_puts(const char *s) {
	while (*s)
		mb_con_putc((uint8_t)*s++);
}

void mb_num(uint16_t v) {
	char b[6];
	uint8_t n = 0;

	if (v == 0) {
		mb_con_putc('0');
		return;
	}
	while (v) {
		b[n++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (n)
		mb_con_putc((uint8_t)b[--n]);
}

void mb_hex8(uint8_t v) {
	static const char hex[] = "0123456789ABCDEF";
	mb_con_putc((uint8_t)hex[v >> 4]);
	mb_con_putc((uint8_t)hex[v & 0x0F]);
}

void mb_remount_cpm(void) { FILE_CMD = FCMD_REMOUNT_CPM; }

/* Blocking line read with local echo and backspace.  The wait ticks are the
 * RNG seed: human keypress timing is the only entropy this machine has. */
static void read_line(char *buf, uint8_t max) {
	uint8_t n = 0;
	int c;

	for (;;) {
		while ((c = mb_con_get()) == 0)
			mb_wait_ticks++;
		if (c == '\r' || c == '\n')
			break;
		if (c == 8 || c == 127) {
			if (n) {
				n--;
				mb_puts("\b \b");
			}
			continue;
		}
		if (c >= 32 && n < (uint8_t)(max - 1)) {
			buf[n++] = (char)c;
			mb_con_putc((uint8_t)c);
		}
	}
	buf[n] = '\0';
	mb_puts("\r\n");
}

/* ---- Directory scan ----------------------------------------------------- */

/* Reset the scan to `dir` and skip the first `skip` entries.  Returns 0 when
 * the directory does not open or holds fewer than `skip` entries.  The list is
 * never held in memory, so any number of entries can be listed. */
static uint8_t dir_start(const char *dir, uint16_t skip) {
	const char *p;

	FILE_RST = 0;
	for (p = dir; *p; p++)
		FILE_NAME = (uint8_t)*p;
	FILE_CMD = FCMD_DIRRESET;
	if (FILE_CMD & FST_ERR)
		return 0;
	while (skip--) {
		FILE_CMD = FCMD_DIRNEXT;
		if ((FILE_CMD & FST_NAME) == 0)
			return 0;
	}
	return 1;
}

/* Advance one entry and copy its name to `out`, setting *is_dir.  0 at end. */
static uint8_t dir_next(char *out, uint8_t max, uint8_t *is_dir) {
	uint8_t i = 0, c, s;

	FILE_CMD = FCMD_DIRNEXT;
	s = FILE_CMD;
	if (is_dir)
		*is_dir = (uint8_t)((s & FST_DIR) != 0);
	if ((s & FST_NAME) == 0)
		return 0;
	while ((c = FILE_NAME) != 0 && i < (uint8_t)(max - 1))
		out[i++] = (char)c;
	out[i] = '\0';
	return i;
}

/* Join `name` onto `dir`; absolute when `name` starts with '/'.  "" = root. */
static void path_join(char *out, uint8_t max, const char *dir,
					  const char *name) {
	uint8_t n = 0;
	const char *p;

	if (name[0] == '/') {
		for (p = name; *p && n < (uint8_t)(max - 1); p++)
			out[n++] = *p;
		out[n] = '\0';
		return;
	}
	if (dir[0]) {
		for (p = dir; *p && n < (uint8_t)(max - 1); p++)
			out[n++] = *p;
		if (n && out[n - 1] != '/' && n < (uint8_t)(max - 1))
			out[n++] = '/';
	}
	for (p = name; *p && n < (uint8_t)(max - 1); p++)
		out[n++] = *p;
	out[n] = '\0';
}

void mb_make_path(char *out, uint8_t max, const char *name) {
	path_join(out, max, mb_dir, name);
}

/* Drop the last path component ("/a/b" -> "/a", "/x" -> ""). */
static void dir_up(char *dir) {
	uint8_t n = 0, last = 0;

	while (dir[n]) {
		if (dir[n] == '/')
			last = n;
		n++;
	}
	if (n == 0)
		return;
	dir[last] = '\0';
}

/* Print entries [page*MB_PAGE_LINES, ...); directories get a trailing '/'.
 * Returns how many were shown. */
static uint8_t show_page(const char *dir, uint16_t page) {
	char nm[MB_NAME_MAX];
	uint16_t idx = (uint16_t)(page * MB_PAGE_LINES);
	uint8_t shown = 0, is_dir = 0;

	if (!dir_start(dir, (uint16_t)(page * MB_PAGE_LINES)))
		return 0;
	while (shown < MB_PAGE_LINES && dir_next(nm, sizeof nm, &is_dir)) {
		mb_num((uint16_t)(idx + 1));
		mb_puts(") ");
		mb_puts(nm);
		if (is_dir)
			mb_con_putc('/');
		mb_puts("\r\n");
		idx++;
		shown++;
	}
	return shown;
}

/* Fetch the 1-based `n`-th entry.  Returns 1 on success. */
static uint8_t nth_entry(const char *dir, uint16_t n, char *out, uint8_t max,
						 uint8_t *is_dir) {
	if (n == 0 || !dir_start(dir, (uint16_t)(n - 1)))
		return 0;
	return dir_next(out, max, is_dir) != 0;
}

static uint8_t dir_has_entries(const char *dir) {
	char nm[MB_NAME_MAX];
	uint8_t d;
	return (uint8_t)(dir_start(dir, 0) && dir_next(nm, sizeof nm, &d) != 0);
}

/* Number of entries in `dir` (files + subdirectories). */
static uint16_t dir_count(const char *dir) {
	char nm[MB_NAME_MAX];
	uint16_t n = 0;
	uint8_t d;

	if (!dir_start(dir, 0))
		return 0;
	while (dir_next(nm, sizeof nm, &d))
		n++;
	return n;
}

/* Case-insensitive .mid / .midi test for play-all (so non-MIDI files are not
 * opened just to fail). */
static uint8_t lower(uint8_t c) {
	return (uint8_t)((c >= 'A' && c <= 'Z') ? c + 32 : c);
}

static uint8_t is_midi_name(const char *nm) {
	const char *dot = 0;
	const char *p;

	for (p = nm; *p; p++)
		if (*p == '.')
			dot = p;
	if (!dot)
		return 0;
	dot++;
	if (lower((uint8_t)dot[0]) != 'm' || lower((uint8_t)dot[1]) != 'i' ||
		lower((uint8_t)dot[2]) != 'd')
		return 0;
	if (dot[3] == '\0')
		return 1;
	return (uint8_t)(lower((uint8_t)dot[3]) == 'i' && dot[4] == '\0');
}

/* ---- Random order ------------------------------------------------------- */

/* xorshift16, seeded from the human keypress timing in read_line(). */
static uint16_t g_rng = 0xACE1u;

static uint16_t rng_next(void) {
	g_rng ^= (uint16_t)(g_rng << 7);
	g_rng ^= (uint16_t)(g_rng >> 9);
	g_rng ^= (uint16_t)(g_rng << 8);
	return g_rng;
}

static uint16_t gcd16(uint16_t a, uint16_t b) {
	while (b) {
		uint16_t t = (uint16_t)(a % b);
		a = b;
		b = t;
	}
	return a;
}

/* A stride coprime to `count`, so `(start + i*stride) % count` visits every
 * entry exactly once (a memory-free shuffle). */
static uint16_t pick_stride(uint16_t count) {
	uint16_t s;

	if (count < 3)
		return 1;
	for (;;) {
		s = (uint16_t)(rng_next() % (uint16_t)(count - 1)) + 1;
		if (gcd16(s, count) == 1)
			return s;
	}
}

/* ---- Browser ------------------------------------------------------------ */

/* Tree browser.  See the file header for the keys.  On a file, returns
 * MB_ACT_FILE with its name in `out` and mb_act_loop reflecting an L prefix;
 * mb_dir is updated in place when navigating. */
uint8_t mb_browse(char *out) {
	char buf[MB_NAME_MAX];
	char nm[MB_NAME_MAX];
	char path[MB_NAME_MAX];
	uint16_t page = 0;
	uint8_t i, digits, shown, is_dir;
	uint16_t n;

	for (;;) {
		shown = show_page(mb_dir, page);
		if (shown == 0 && page > 0) { /* ran past the end: step back */
			page--;
			continue;
		}
		mb_puts("Dir ");
		mb_puts(mb_dir[0] ? mb_dir : "/");
		mb_puts("  page ");
		mb_num((uint16_t)(page + 1));
		mb_puts("  (num, .., n/p, a/A all, r/R rand, *num loop, q): ");
		read_line(buf, sizeof buf);

		mb_act_loop = 0;

		if ((buf[0] == 'q' || buf[0] == 'Q') && buf[1] == '\0')
			return MB_ACT_QUIT;
		if (buf[0] == '.' && buf[1] == '.' && buf[2] == '\0') {
			dir_up(mb_dir);
			page = 0;
			continue;
		}
		if ((buf[0] == 'n' || buf[0] == 'N') && buf[1] == '\0') {
			page++;
			continue;
		}
		if ((buf[0] == 'p' || buf[0] == 'P') && buf[1] == '\0') {
			if (page)
				page--;
			continue;
		}
		if (buf[1] == '\0' && (buf[0] == 'a' || buf[0] == 'A'))
			return (buf[0] == 'a') ? MB_ACT_ALL : MB_ACT_ALLR;
		if (buf[1] == '\0' && (buf[0] == 'r' || buf[0] == 'R'))
			return (buf[0] == 'r') ? MB_ACT_RAND : MB_ACT_RANDR;
		if (buf[0] == '\0')
			continue; /* blank: redraw */

		/* *<spec>: play the entry (number or name) on repeat.  The '*' is
		 * not a legal FAT name character, so it cannot be a filename.
		 * Strip the prefix and fall through to the number/name handling. */
		if (buf[0] == '*') {
			if (buf[1] == '\0') {
				mb_puts("Use *<num> or *<name> to loop one file.\r\n");
				continue;
			}
			for (i = 0; buf[i + 1]; i++)
				buf[i] = buf[i + 1];
			buf[i] = '\0';
			mb_act_loop = 1;
		}

		digits = 1;
		for (i = 0; buf[i]; i++)
			if (buf[i] < '0' || buf[i] > '9') {
				digits = 0;
				break;
			}
		if (digits) {
			n = 0;
			for (i = 0; buf[i]; i++)
				n = (uint16_t)(n * 10 + (uint16_t)(buf[i] - '0'));
			if (!nth_entry(mb_dir, n, nm, sizeof nm, &is_dir)) {
				mb_puts("No such entry.\r\n");
				mb_act_loop = 0;
				continue;
			}
			if (is_dir) { /* descend, staying inside mb_dir */
				path_join(path, sizeof path, mb_dir, nm);
				for (i = 0; path[i] && i < (uint8_t)(sizeof mb_dir - 1); i++)
					mb_dir[i] = path[i];
				mb_dir[i] = '\0';
				page = 0;
				mb_act_loop = 0;
				continue;
			}
			for (i = 0; nm[i]; i++)
				out[i] = nm[i];
			out[i] = '\0';
			return MB_ACT_FILE;
		}
		/* anything else is a filename (relative to mb_dir, or absolute) */
		for (i = 0; buf[i]; i++)
			out[i] = buf[i];
		out[i] = '\0';
		return MB_ACT_FILE;
	}
}

/* Start in /MIDI when it exists, else the card root.  Returns 1 when the
 * chosen directory has entries, 0 otherwise. */
uint8_t mb_pick_start_dir(void) {
	mb_dir[0] = '\0';
	if (dir_has_entries(MIDI_DIR)) {
		uint8_t i = 0;
		const char *p = MIDI_DIR;
		while (p[i] && i < (uint8_t)(sizeof mb_dir - 1)) {
			mb_dir[i] = p[i];
			i++;
		}
		mb_dir[i] = '\0';
		return 1;
	}
	return dir_has_entries("");
}

/* ---- Playback ----------------------------------------------------------- */

#define PLAY_END  1 /* song reached its end sentinel */
#define PLAY_STOP 0 /* user pressed q/ESC */
#define PLAY_FAIL 2 /* could not open */
#define PLAY_NEXT 3 /* user pressed n */

/* Open and play one file once.  Returns PLAY_*. */
static uint8_t play_one(const char *path) {
	const char *p;
	uint8_t s;

	mb_puts("Opening ");
	mb_puts(path);
	mb_puts(" ... ");
	FILE_RST = 0;
	for (p = path; *p; p++)
		FILE_NAME = (uint8_t)*p;
	FILE_CMD = FCMD_OPEN_MIDI;
	s = FILE_CMD;
	if ((s & FST_OPEN) == 0) {
		mb_puts("failed (status 0x");
		mb_hex8(s);
		mb_puts(")\r\n\r\n");
		FILE_CMD = FCMD_CLOSE;
		return PLAY_FAIL;
	}
	mb_puts("ok\r\nPlaying (q/ESC stop, n next)...\r\n");
#ifdef MIDI_UI
	midi_ui_start(path); /* draw the dashboard before the stream is read, so
						  * the scheduler anchors tick 0 after it */
#endif
	midi_play();
#ifdef MIDI_UI
	midi_ui_stop();
#endif
	FILE_CMD = FCMD_CLOSE;
	if (midi_stopped == 2) {
		mb_puts("\r\nNext.\r\n\r\n");
		return PLAY_NEXT;
	}
	if (midi_stopped) {
		mb_puts("\r\nStopped.\r\n\r\n");
		return PLAY_STOP;
	}
	return PLAY_END;
}

void mb_play_file(const char *path, uint8_t loop) {
	uint8_t r;

	for (;;) {
		r = play_one(path);
		if (!loop || r != PLAY_END)
			break;
	}
}

/* Files started by the last mb_play_dir() call, for the "no files" message. */
static uint16_t g_played;

/* Depth-first play of every .mid file at or under `dir`, descending into
 * subdirectories.  `dir` is a caller-owned buffer that stays valid across the
 * recursion (each frame's `path` holds the child path handed to the next
 * level), so mb_dir is left untouched and the browser stays where it was.
 * Each directory keeps the coprime-stride shuffle when random_order.
 * Returns 1 when the user stopped playback. */
static uint8_t play_tree(const char *dir, uint8_t random_order) {
	char nm[MB_NAME_MAX];
	char path[MB_NAME_MAX + 16];
	uint16_t count, i, idx;
	uint32_t start = 0, step = 1;
	uint8_t d, stop;

	if (!random_order) {
		if (!dir_start(dir, 0))
			return 0;
		while (dir_next(nm, sizeof nm, &d)) {
			path_join(path, sizeof path, dir, nm);
			if (d) {
				FILE_CMD = FCMD_DIRPUSH;
				stop = play_tree(path, 0);
				FILE_CMD = FCMD_DIRPOP;
				if (stop)
					return 1;
			} else if (is_midi_name(nm)) {
				g_played++;
				if (play_one(path) == PLAY_STOP)
					return 1;
			}
		}
		return 0;
	}

	count = dir_count(dir);
	if (count == 0)
		return 0;
	g_rng = (uint16_t)(g_rng ^ mb_wait_ticks ^ count ^ 0x9E37u);
	if (g_rng == 0)
		g_rng = 1;
	step = pick_stride(count);
	start = (uint32_t)(rng_next() % count);

	for (i = 0; i < count; i++) {
		idx = (uint16_t)((start + (uint32_t)i * step) % count);
		if (!nth_entry(dir, (uint16_t)(idx + 1), nm, sizeof nm, &d))
			continue;
		path_join(path, sizeof path, dir, nm);
		if (d) {
			FILE_CMD = FCMD_DIRPUSH;
			stop = play_tree(path, random_order);
			FILE_CMD = FCMD_DIRPOP;
			if (stop)
				return 1;
		} else if (is_midi_name(nm)) {
			g_played++;
			if (play_one(path) == PLAY_STOP)
				return 1;
		}
	}
	return 0;
}

/* Play every .mid file in the current directory and all of its subdirectories,
 * in order or shuffled, optionally looping until the user stops. */
void mb_play_dir(uint8_t flags) {
	uint8_t random_order = (uint8_t)((flags & MB_PLAY_RAND) != 0);
	uint8_t repeat = (uint8_t)((flags & MB_PLAY_REPEAT) != 0);

	if (dir_count(mb_dir) == 0) {
		mb_puts("Empty directory.\r\n\r\n");
		return;
	}

	g_played = 0;
	do {
		if (play_tree(mb_dir, random_order))
			break;
		if (g_played == 0) /* nothing playable under here */
			break;
	} while (repeat);

	if (g_played == 0)
		mb_puts("No files found.\r\n\r\n");
}
