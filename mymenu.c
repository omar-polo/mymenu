/*
 * Copyright (c) 2018, 2019, 2020, 2022 Omar Polo <op@omarpolo.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <ctype.h> /* isalnum */
#include <err.h>
#include <errno.h>
#include <limits.h>
#include <locale.h> /* setlocale */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h> /* strdup, strlen */
#include <sysexits.h>
#include <unistd.h>

#include <X11/Xcms.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>

#include <X11/extensions/Xinerama.h>

#define RESNAME "MyMenu"
#define RESCLASS "mymenu"

#define SYM_BUF_SIZE 4

#define DEFFONT "monospace"

#define ARGS "Aahmve:p:P:l:f:W:H:x:y:b:B:t:T:c:C:s:S:d:G:g:I:i:J:j:"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define EXPANDBITS(x) (((x & 0xf0) * 0x100) | (x & 0x0f) * 0x10)

#define INNER_HEIGHT(r) (r->height - r->borders[0] - r->borders[2])
#define INNER_WIDTH(r) (r->width - r->borders[1] - r->borders[3])

/* The states of the event loop */
enum state { LOOPING, OK_LOOP, OK, ERR };

/*
 * For the drawing-related function. The text to be rendere could be
 * the prompt, a completion or a highlighted completion
 */
enum obj_type { PROMPT, COMPL, COMPL_HIGH };

/* These are the possible action to be performed after user input. */
enum action {
	NO_OP,
	EXIT,
	CONFIRM,
	CONFIRM_CONTINUE,
	NEXT_COMPL,
	PREV_COMPL,
	DEL_CHAR,
	DEL_WORD,
	DEL_LINE,
	ADD_CHAR,
	TOGGLE_FIRST_SELECTED,
	SCROLL_DOWN,
	SCROLL_UP,
};

/* A big set of values that needs to be carried around for drawing. A
 * big struct to rule them all */
struct rendering {
	Display *d; /* Connection to xorg */
	Window w;
	XIM xim;
	int width;
	int height;
	int p_padding[4];
	int c_padding[4];
	int ch_padding[4];
	int x_zero; /* the "zero" on the x axis (may not be exactly 0 'cause
		       the borders) */
	int y_zero; /* like x_zero but for the y axis */

	int offset; /* scroll offset */

	short free_text;
	short first_selected;
	short multiple_select;

	/* four border width */
	int borders[4];
	int p_borders[4];
	int c_borders[4];
	int ch_borders[4];

	short horizontal_layout;

	/* prompt */
	char *ps1;
	int ps1len;
	int ps1w; /* ps1 width */
	int ps1h; /* ps1 height */

	int text_height; /* cache for the vertical layout */

	XIC xic;

	/* colors */
	GC fgs[4];
	GC bgs[4];
	GC borders_bg[4];
	GC p_borders_bg[4];
	GC c_borders_bg[4];
	GC ch_borders_bg[4];
	XftFont *font;
	XftDraw *xftdraw;
	XftColor xft_colors[3];
};

struct completion {
	char *completion;
	char *rcompletion;

	/*
	 * The X (or Y, depending on the layour) at which the item is
	 * rendered
	 */
	int offset;
};

/* Wrap the linked list of completions */
struct completions {
	struct completion *completions;
	ssize_t selected;
	size_t length;
};

/* idea stolen from lemonbar;  ty lemonboy */
typedef union {
	struct {
		uint8_t b;
		uint8_t g;
		uint8_t r;
		uint8_t a;
	} rgba;
	uint32_t v;
} rgba_t;

/* Return a newly allocated (and empty) completion list */
static struct completions *
compls_new(size_t length)
{
	struct completions *cs = malloc(sizeof(struct completions));

	if (cs == NULL)
		return cs;

	cs->completions = calloc(length, sizeof(struct completion));
	if (cs->completions == NULL) {
		free(cs);
		return NULL;
	}

	cs->selected = -1;
	cs->length = length;
	return cs;
}

/* Delete the wrapper and the whole list */
static void
compls_delete(struct completions *cs)
{
	if (cs == NULL)
		return;

	free(cs->completions);
	free(cs);
}

/*
 * Create a completion list from a text and the list of possible
 * completions (null terminated). Expects a non-null `cs'. `lines' and
 * `vlines' should have the same length OR `vlines' is NULL.
 */
static void
filter(struct completions *cs, char *text, char **lines, char **vlines)
{
	size_t index = 0;
	size_t matching = 0;
	char *l;

	if (vlines == NULL)
		vlines = lines;

	while (1) {
		if (lines[index] == NULL)
			break;

		l = vlines[index] != NULL ? vlines[index] : lines[index];

		if (strcasestr(l, text) != NULL) {
			struct completion *c = &cs->completions[matching];
			c->completion = l;
			c->rcompletion = lines[index];
			matching++;
		}

		index++;
	}
	cs->length = matching;
	cs->selected = -1;
}

/* Update the given completion */
static void
update_completions(struct completions *cs, char *text, char **lines,
    char **vlines, short first_selected)
{
	filter(cs, text, lines, vlines);
	if (first_selected && cs->length > 0)
		cs->selected = 0;
}

/*
 * Select the next or previous selection and update some state. `text'
 * will be updated with the text of the completion and `textlen' with
 * the new length. If the memory cannot be allocated `status' will be
 * set to `ERR'.
 */
static void
complete(struct completions *cs, short first_selected, short p,
    char **text, int *textlen, enum state *status)
{
	struct completion *n;
	int index;

	if (cs == NULL || cs->length == 0)
		return;

	/*
	 * If the first is always selected and the first entry is
	 * different from the text, expand the text and return
	 */
	if (first_selected &&
	    cs->selected == 0 &&
	    strcmp(cs->completions->completion, *text) != 0 &&
	    !p) {
		free(*text);
		*text = strdup(cs->completions->completion);
		if (text == NULL) {
			*status = ERR;
			return;
		}
		*textlen = strlen(*text);
		return;
	}

	index = cs->selected;

	if (index == -1 && p)
		index = 0;
	index = cs->selected = (cs->length + (p ? index - 1 : index + 1))
		% cs->length;

	n = &cs->completions[cs->selected];

	free(*text);
	*text = strdup(n->completion);
	if (text == NULL) {
		fprintf(stderr, "Memory allocation error!\n");
		*status = ERR;
		return;
	}
	*textlen = strlen(*text);
}

/* Push the character c at the end of the string pointed by p */
static int
pushc(char **p, int maxlen, char c)
{
	int len;

	len = strnlen(*p, maxlen);
	if (!(len < maxlen - 2)) {
		char *newptr;

		maxlen += maxlen >> 1;
		newptr = realloc(*p, maxlen);
		if (newptr == NULL) /* bad */
			return -1;
		*p = newptr;
	}

	(*p)[len] = c;
	(*p)[len + 1] = '\0';
	return maxlen;
}

/*
 * Remove the last rune from the *UTF-8* string! This is different
 * from just setting the last byte to 0 (in some cases ofc). Return a
 * pointer (e) to the last nonzero char. If e < p then p is empty!
 */
static char *
popc(char *p)
{
	int len = strlen(p);
	char *e;

	if (len == 0)
		return p;

	e = p + len - 1;

	do {
		char c = *e;

		*e = '\0';
		e -= 1;

		/*
		 * If c is a starting byte (11......) or is under
		 * U+007F we're done.
		 */
		if (((c & 0x80) && (c & 0x40)) || !(c & 0x80))
			break;
	} while (e >= p);

	return e;
}

/* Remove the last word plus trailing white spaces from the given string */
static void
popw(char *w)
{
	short in_word = 1;

	if (*w == '\0')
		return;

	while (1) {
		char *e = popc(w);

		if (e < w)
			return;

		if (in_word && isspace(*e))
			in_word = 0;

		if (!in_word && !isspace(*e))
			return;
	}
}

/*
 * If the string is surrounded by quates (`"') remove them and replace
 * every `\"' in the string with a single double-quote.
 */
static char *
normalize_str(const char *str)
{
	int len, p;
	char *s;

	if ((len = strlen(str)) == 0)
		return NULL;

	if ((s = calloc(len, sizeof(char))) == NULL)
		err(1, "calloc");
	p = 0;

	while (*str) {
		char c = *str;

		if (*str == '\\') {
			if (*(str + 1)) {
				s[p] = *(str + 1);
				p++;
				str += 2; /* skip this and the next char */
				continue;
			} else
				break;
		}
		if (c == '"') {
			str++; /* skip only this char */
			continue;
		}
		s[p] = c;
		p++;
		str++;
	}

	return s;
}

static char **
readlines(size_t *lineslen)
{
	size_t len = 0, cap = 0;
	size_t linesize = 0;
	ssize_t linelen;
	char *line = NULL, **lines = NULL;

	while ((linelen = getline(&line, &linesize, stdin)) != -1) {
		if (linelen != 0 && line[linelen-1] == '\n')
			line[linelen-1] = '\0';

		if (len == cap) {
			size_t newcap;
			void *t;

			newcap = MAX(cap * 1.5, 32);
			t = recallocarray(lines, cap, newcap, sizeof(char *));
			if (t == NULL)
				err(1, "recallocarray");
			cap = newcap;
			lines = t;
		}

		if ((lines[len++] = strdup(line)) == NULL)
			err(1, "strdup");
	}

	if (ferror(stdin))
		err(1, "getline");
	free(line);

	*lineslen = len;
	return lines;
}

/*
 * Compute the dimensions of the string str once rendered.
 * It'll return the width and set ret_width and ret_height if not NULL
 */
static int
text_extents(char *str, int len, struct rendering *r, int *ret_width,
    int *ret_height)
{
	int height, width;
	XGlyphInfo gi;
	XftTextExtentsUtf8(r->d, r->font, str, len, &gi);
	height = r->font->ascent - r->font->descent;
	width = gi.width - gi.x;

	if (ret_width != NULL)
		*ret_width = width;
	if (ret_height != NULL)
		*ret_height = height;
	return width;
}

static void
draw_string(char *str, int len, int x, int y, struct rendering *r,
    enum obj_type tt)
{
	XftColor xftcolor;
	if (tt == PROMPT)
		xftcolor = r->xft_colors[0];
	if (tt == COMPL)
		xftcolor = r->xft_colors[1];
	if (tt == COMPL_HIGH)
		xftcolor = r->xft_colors[2];

	XftDrawStringUtf8(r->xftdraw, &xftcolor, r->font, x, y, str, len);
}

/* Duplicate the string and substitute every space with a 'n` */
static char *
strdupn(char *str)
{
	char *t, *dup;

	if (str == NULL || *str == '\0')
		return NULL;

	if ((dup = strdup(str)) == NULL)
		return NULL;

	for (t = dup; *t; ++t) {
		if (*t == ' ')
			*t = 'n';
	}

	return dup;
}

static int
draw_v_box(struct rendering *r, int y, char *prefix, int prefix_width,
    enum obj_type t, char *text)
{
	GC *border_color, bg;
	int *padding, *borders;
	int ret = 0, inner_width, inner_height, x;

	switch (t) {
	case PROMPT:
		border_color = r->p_borders_bg;
		padding = r->p_padding;
		borders = r->p_borders;
		bg = r->bgs[0];
		break;
	case COMPL:
		border_color = r->c_borders_bg;
		padding = r->c_padding;
		borders = r->c_borders;
		bg = r->bgs[1];
		break;
	case COMPL_HIGH:
		border_color = r->ch_borders_bg;
		padding = r->ch_padding;
		borders = r->ch_borders;
		bg = r->bgs[2];
		break;
	}

	ret = borders[0] + padding[0] + r->text_height + padding[2] + borders[2];

	inner_width = INNER_WIDTH(r) - borders[1] - borders[3];
	inner_height = padding[0] + r->text_height + padding[2];

	/* Border top */
	XFillRectangle(r->d, r->w, border_color[0], r->x_zero, y, r->width,
	    borders[0]);

	/* Border right */
	XFillRectangle(r->d, r->w, border_color[1],
	    r->x_zero + INNER_WIDTH(r) - borders[1], y, borders[1], ret);

	/* Border bottom */
	XFillRectangle(r->d, r->w, border_color[2], r->x_zero,
	    y + borders[0] + padding[0] + r->text_height + padding[2],
	    r->width, borders[2]);

	/* Border left */
	XFillRectangle(r->d, r->w, border_color[3], r->x_zero, y, borders[3],
	    ret);

	/* bg */
	x = r->x_zero + borders[3];
	y += borders[0];
	XFillRectangle(r->d, r->w, bg, x, y, inner_width, inner_height);

	/* content */
	y += padding[0] + r->text_height;
	x += padding[3];
	if (prefix != NULL) {
		draw_string(prefix, strlen(prefix), x, y, r, t);
		x += prefix_width;
	}
	draw_string(text, strlen(text), x, y, r, t);

	return ret;
}

static int
draw_h_box(struct rendering *r, int x, char *prefix, int prefix_width,
    enum obj_type t, char *text)
{
	GC *border_color, bg;
	int *padding, *borders;
	int ret = 0, inner_width, inner_height, y, text_width;

	switch (t) {
	case PROMPT:
		border_color = r->p_borders_bg;
		padding = r->p_padding;
		borders = r->p_borders;
		bg = r->bgs[0];
		break;
	case COMPL:
		border_color = r->c_borders_bg;
		padding = r->c_padding;
		borders = r->c_borders;
		bg = r->bgs[1];
		break;
	case COMPL_HIGH:
		border_color = r->ch_borders_bg;
		padding = r->ch_padding;
		borders = r->ch_borders;
		bg = r->bgs[2];
		break;
	}

	if (padding[0] < 0 || padding[2] < 0) {
		padding[0] = INNER_HEIGHT(r) - borders[0] - borders[2]
			- r->text_height;
		padding[0] /= 2;

		padding[2] = padding[0];
	}

	/* If they are still lesser than 0, set 'em to 0 */
	if (padding[0] < 0 || padding[2] < 0)
		padding[0] = padding[2] = 0;

	/* Get the text width */
	text_extents(text, strlen(text), r, &text_width, NULL);
	if (prefix != NULL)
		text_width += prefix_width;

	ret = borders[3] + padding[3] + text_width + padding[1] + borders[1];

	inner_width = padding[3] + text_width + padding[1];
	inner_height = INNER_HEIGHT(r) - borders[0] - borders[2];

	/* Border top */
	XFillRectangle(r->d, r->w, border_color[0], x, r->y_zero, ret,
	    borders[0]);

	/* Border right */
	XFillRectangle(r->d, r->w, border_color[1],
	    x + borders[3] + inner_width, r->y_zero, borders[1],
	    INNER_HEIGHT(r));

	/* Border bottom */
	XFillRectangle(r->d, r->w, border_color[2], x,
	    r->y_zero + INNER_HEIGHT(r) - borders[2], ret,
	    borders[2]);

	/* Border left */
	XFillRectangle(r->d, r->w, border_color[3], x, r->y_zero, borders[3],
	    INNER_HEIGHT(r));

	/* bg */
	x += borders[3];
	y = r->y_zero + borders[0];
	XFillRectangle(r->d, r->w, bg, x, y, inner_width, inner_height);

	/* content */
	y += padding[0] + r->text_height;
	x += padding[3];
	if (prefix != NULL) {
		draw_string(prefix, strlen(prefix), x, y, r, t);
		x += prefix_width;
	}
	draw_string(text, strlen(text), x, y, r, t);

	return ret;
}

/*
 * ,-----------------------------------------------------------------,
 * | 20 char text     | completion | completion | completion | compl |
 *  `-----------------------------------------------------------------'
 */
static void
draw_horizontally(struct rendering *r, char *text, struct completions *cs)
{
	size_t i;
	int x = r->x_zero;

	/* Draw the prompt */
	x += draw_h_box(r, x, r->ps1, r->ps1w, PROMPT, text);

	for (i = r->offset; i < cs->length; ++i) {
		enum obj_type t;

		if (cs->selected == (ssize_t)i)
			t = COMPL_HIGH;
		else
			t = COMPL;

		cs->completions[i].offset = x;

		x += draw_h_box(r, x, NULL, 0, t,
		    cs->completions[i].completion);

		if (x > INNER_WIDTH(r))
			break;
	}

	for (i += 1; i < cs->length; ++i)
		cs->completions[i].offset = -1;
}

/*
 * ,-----------------------------------------------------------------,
 * |  prompt                                                         |
 * |-----------------------------------------------------------------|
 * |  completion                                                     |
 * |-----------------------------------------------------------------|
 * |  completion                                                     |
 * `-----------------------------------------------------------------'
 */
static void
draw_vertically(struct rendering *r, char *text, struct completions *cs)
{
	size_t i;
	int y = r->y_zero;

	y += draw_v_box(r, y, r->ps1, r->ps1w, PROMPT, text);

	for (i = r->offset; i < cs->length; ++i) {
		enum obj_type t;

		if (cs->selected == (ssize_t)i)
			t = COMPL_HIGH;
		else
			t = COMPL;

		cs->completions[i].offset = y;

		y += draw_v_box(r, y, NULL, 0, t,
		    cs->completions[i].completion);

		if (y > INNER_HEIGHT(r))
			break;
	}

	for (i += 1; i < cs->length; ++i)
		cs->completions[i].offset = -1;
}

static void
draw(struct rendering *r, char *text, struct completions *cs)
{
	/* Draw the background */
	XFillRectangle(r->d, r->w, r->bgs[1], r->x_zero, r->y_zero,
	    INNER_WIDTH(r), INNER_HEIGHT(r));

	/* Draw the contents */
	if (r->horizontal_layout)
		draw_horizontally(r, text, cs);
	else
		draw_vertically(r, text, cs);

	/* Draw the borders */
	if (r->borders[0] != 0)
		XFillRectangle(r->d, r->w, r->borders_bg[0], 0, 0, r->width,
		    r->borders[0]);

	if (r->borders[1] != 0)
		XFillRectangle(r->d, r->w, r->borders_bg[1],
		    r->width - r->borders[1], 0, r->borders[1],
		    r->height);

	if (r->borders[2] != 0)
		XFillRectangle(r->d, r->w, r->borders_bg[2], 0,
		    r->height - r->borders[2], r->width, r->borders[2]);

	if (r->borders[3] != 0)
		XFillRectangle(r->d, r->w, r->borders_bg[3], 0, 0,
		    r->borders[3], r->height);

	/* render! */
	XFlush(r->d);
}

/* Set some WM stuff */
static void
set_win_atoms_hints(Display *d, Window w, int width, int height)
{
	Atom type;
	XClassHint *class_hint;
	XSizeHints *size_hint;

	type = XInternAtom(d, "_NET_WM_WINDOW_TYPE_DOCK", 0);
	XChangeProperty(d, w, XInternAtom(d, "_NET_WM_WINDOW_TYPE", 0),
	    XInternAtom(d, "ATOM", 0), 32, PropModeReplace,
	    (unsigned char *)&type, 1);

	/* some window managers honor this properties */
	type = XInternAtom(d, "_NET_WM_STATE_ABOVE", 0);
	XChangeProperty(d, w, XInternAtom(d, "_NET_WM_STATE", 0),
	    XInternAtom(d, "ATOM", 0), 32, PropModeReplace,
	    (unsigned char *)&type, 1);

	type = XInternAtom(d, "_NET_WM_STATE_FOCUSED", 0);
	XChangeProperty(d, w, XInternAtom(d, "_NET_WM_STATE", 0),
	    XInternAtom(d, "ATOM", 0), 32, PropModeAppend,
	    (unsigned char *)&type, 1);

	/* Setting window hints */
	class_hint = XAllocClassHint();
	if (class_hint == NULL) {
		fprintf(stderr, "Could not allocate memory for class hint\n");
		exit(EX_UNAVAILABLE);
	}

	class_hint->res_name = RESNAME;
	class_hint->res_class = RESCLASS;
	XSetClassHint(d, w, class_hint);
	XFree(class_hint);

	size_hint = XAllocSizeHints();
	if (size_hint == NULL) {
		fprintf(stderr, "Could not allocate memory for size hint\n");
		exit(EX_UNAVAILABLE);
	}

	size_hint->flags = PMinSize | PBaseSize;
	size_hint->min_width = width;
	size_hint->base_width = width;
	size_hint->min_height = height;
	size_hint->base_height = height;

	XFlush(d);
}

/* Get the width and height of the window `w' */
static void
get_wh(Display *d, Window *w, int *width, int *height)
{
	XWindowAttributes win_attr;

	XGetWindowAttributes(d, *w, &win_attr);
	*height = win_attr.height;
	*width = win_attr.width;
}

/* find the current xinerama monitor if possible */
static void
findmonitor(Display *d, int *x, int *y, int *width, int *height)
{
	XineramaScreenInfo *info;
	Window rr;
	Window root;
	int screens, monitors, i;
	int rootx, rooty, winx, winy;
	unsigned int mask;
	short res;

	if (!XineramaIsActive(d))
		return;

	screens = XScreenCount(d);
	for (i = 0; i < screens; ++i) {
		root = XRootWindow(d, i);
		res = XQueryPointer(d, root, &rr, &rr, &rootx, &rooty, &winx,
		    &winy, &mask);
		if (res)
			break;
	}

	if (!res)
		return;

	/* Now find in which monitor the mice is */
	info = XineramaQueryScreens(d, &monitors);
	if (info == NULL)
		return;

	for (i = 0; i < monitors; ++i) {
		if (info[i].x_org <= rootx &&
		    rootx <= (info[i].x_org + info[i].width) &&
		    info[i].y_org <= rooty &&
		    rooty <= (info[i].y_org + info[i].height)) {
			*x = info[i].x_org;
			*y = info[i].y_org;
			*width = info[i].width;
			*height = info[i].height;
			break;
		}
	}

	XFree(info);
}

static int
grabfocus(Display *d, Window w)
{
	int i;
	for (i = 0; i < 100; ++i) {
		Window focuswin;
		int revert_to_win;

		XGetInputFocus(d, &focuswin, &revert_to_win);

		if (focuswin == w)
			return 1;

		XSetInputFocus(d, w, RevertToParent, CurrentTime);
		usleep(1000);
	}
	return 0;
}

/*
 * I know this may seem a little hackish BUT is the only way I managed
 * to actually grab that goddam keyboard. Only one call to
 * XGrabKeyboard does not always end up with the keyboard grabbed!
 */
static int
take_keyboard(Display *d, Window w)
{
	int i;
	for (i = 0; i < 100; i++) {
		if (XGrabKeyboard(d, w, 1, GrabModeAsync, GrabModeAsync,
		    CurrentTime) == GrabSuccess)
			return 1;
		usleep(1000);
	}
	fprintf(stderr, "Cannot grab keyboard\n");
	return 0;
}

static unsigned long
parse_color(const char *str, const char *def)
{
	size_t len;
	rgba_t tmp;
	char *ep;

	if (str == NULL)
		goto err;

	len = strlen(str);

	/* +1 for the # ath the start */
	if (*str != '#' || len > 9 || len < 4)
		goto err;
	++str; /* skip the # */

	errno = 0;
	tmp = (rgba_t)(uint32_t)strtoul(str, &ep, 16);

	if (errno)
		goto err;

	switch (len - 1) {
	case 3:
		/* expand #rgb -> #rrggbb */
		tmp.v = (tmp.v & 0xf00) * 0x1100 | (tmp.v & 0x0f0) * 0x0110
			| (tmp.v & 0x00f) * 0x0011;
	case 6:
		/* assume 0xff opacity */
		tmp.rgba.a = 0xff;
		break;
	} /* colors in #aarrggbb need no adjustments */

	/* premultiply the alpha */
	if (tmp.rgba.a) {
		tmp.rgba.r = (tmp.rgba.r * tmp.rgba.a) / 255;
		tmp.rgba.g = (tmp.rgba.g * tmp.rgba.a) / 255;
		tmp.rgba.b = (tmp.rgba.b * tmp.rgba.a) / 255;
		return tmp.v;
	}

	return 0U;

err:
	fprintf(stderr, "Invalid color: \"%s\".\n", str);
	if (def != NULL)
		return parse_color(def, NULL);
	else
		return 0U;
}

/*
 * Given a string try to parse it as a number or return `def'.
 */
static int
parse_integer(const char *str, int def)
{
	const char *errstr;
	int i;

	i = strtonum(str, INT_MIN, INT_MAX, &errstr);
	if (errstr != NULL) {
		warnx("'%s' is %s; using %d as default", str, errstr, def);
		return def;
	}

	return i;
}

/*
 * Like parse_integer but recognize the percentages (i.e. strings
 * ending with `%')
 */
static int
parse_int_with_percentage(const char *str, int default_value, int max)
{
	int len = strlen(str);

	if (len > 0 && str[len - 1] == '%') {
		int val;
		char *cpy;

		if ((cpy = strdup(str)) == NULL)
			err(1, "strdup");

		cpy[len - 1] = '\0';
		val = parse_integer(cpy, default_value);
		free(cpy);
		return val * max / 100;
	}

	return parse_integer(str, default_value);
}

static void
get_mouse_coords(Display *d, int *x, int *y)
{
	Window w, root;
	int i;
	unsigned int u;

	*x = *y = 0;
	root = DefaultRootWindow(d);

	if (!XQueryPointer(d, root, &root, &w, x, y, &i, &i, &u)) {
		for (i = 0; i < ScreenCount(d); ++i) {
			if (root == RootWindow(d, i))
				break;
		}
	}
}

/*
 * Like parse_int_with_percentage but understands some special values:
 * - middle that is (max-self)/2
 * - center = middle
 * - start  that is 0
 * - end    that is (max-self)
 * - mx     x coordinate of the mouse
 * - my     y coordinate of the mouse
 */
static int
parse_int_with_pos(Display *d, const char *str, int default_value, int max,
    int self)
{
	if (!strcmp(str, "start"))
		return 0;
	if (!strcmp(str, "middle") || !strcmp(str, "center"))
		return (max - self) / 2;
	if (!strcmp(str, "end"))
		return max - self;
	if (!strcmp(str, "mx") || !strcmp(str, "my")) {
		int x, y;

		get_mouse_coords(d, &x, &y);
		if (!strcmp(str, "mx"))
			return x - 1;
		else
			return y - 1;
	}
	return parse_int_with_percentage(str, default_value, max);
}

/* Parse a string like a CSS value. */
/* TODO: harden a bit this function */
static int
parse_csslike(const char *str, char **ret)
{
	int i, j;
	char *s, *token;
	short any_null;

	memset(ret, 0, 4 * sizeof(*ret));

	s = strdup(str);
	if (s == NULL)
		return -1;

	for (i = 0; (token = strsep(&s, " ")) != NULL && i < 4; ++i)
		ret[i] = strdup(token);

	if (i == 1)
		for (j = 1; j < 4; j++)
			ret[j] = strdup(ret[0]);

	if (i == 2) {
		ret[2] = strdup(ret[0]);
		ret[3] = strdup(ret[1]);
	}

	if (i == 3)
		ret[3] = strdup(ret[1]);

	/*
	 * before we didn't check for the return type of strdup, here
	 * we will
	 */

	any_null = 0;
	for (i = 0; i < 4; ++i)
		any_null = ret[i] == NULL || any_null;

	if (any_null)
		for (i = 0; i < 4; ++i)
			free(ret[i]);

	if (i == 0 || any_null) {
		free(s);
		return -1;
	}

	return 1;
}

/*
 * Given an event, try to understand what the users wants. If the
 * return value is ADD_CHAR then `input' is a pointer to a string that
 * will need to be free'ed later.
 */
static enum action
parse_event(Display *d, XKeyPressedEvent *ev, XIC xic, char **input)
{
	char str[SYM_BUF_SIZE] = { 0 };
	Status s;

	if (ev->keycode == XKeysymToKeycode(d, XK_BackSpace))
		return DEL_CHAR;

	if (ev->keycode == XKeysymToKeycode(d, XK_Tab))
		return ev->state & ShiftMask ? PREV_COMPL : NEXT_COMPL;

	if (ev->keycode == XKeysymToKeycode(d, XK_Return))
		return CONFIRM;

	if (ev->keycode == XKeysymToKeycode(d, XK_Escape))
		return EXIT;

	/* Try to read what key was pressed */
	s = 0;
	Xutf8LookupString(xic, ev, str, SYM_BUF_SIZE, 0, &s);
	if (s == XBufferOverflow) {
		fprintf(stderr,
		    "Buffer overflow when trying to create keyboard "
		    "symbol map.\n");
		return EXIT;
	}

	if (ev->state & ControlMask) {
		if (!strcmp(str, "")) /* C-u */
			return DEL_LINE;
		if (!strcmp(str, "")) /* C-w */
			return DEL_WORD;
		if (!strcmp(str, "")) /* C-h */
			return DEL_CHAR;
		if (!strcmp(str, "\r")) /* C-m */
			return CONFIRM_CONTINUE;
		if (!strcmp(str, "")) /* C-p */
			return PREV_COMPL;
		if (!strcmp(str, "")) /* C-n */
			return NEXT_COMPL;
		if (!strcmp(str, "")) /* C-c */
			return EXIT;
		if (!strcmp(str, "\t")) /* C-i */
			return TOGGLE_FIRST_SELECTED;
	}

	*input = strdup(str);
	if (*input == NULL) {
		fprintf(stderr, "Error while allocating memory for key.\n");
		return EXIT;
	}

	return ADD_CHAR;
}

static void
confirm(enum state *status, struct rendering *r, struct completions *cs,
    char **text, int *textlen)
{
	if ((cs->selected != -1) || (cs->length > 0 && r->first_selected)) {
		/* if there is something selected expand it and return */
		int index = cs->selected == -1 ? 0 : cs->selected;
		struct completion *c = cs->completions;
		char *t;

		while (1) {
			if (index == 0)
				break;
			c++;
			index--;
		}

		t = c->rcompletion;
		free(*text);
		*text = strdup(t);

		if (*text == NULL) {
			fprintf(stderr, "Memory allocation error\n");
			*status = ERR;
		}

		*textlen = strlen(*text);
		return;
	}

	if (!r->free_text) /* cannot accept arbitrary text */
		*status = LOOPING;
}

/*
 * cs: completion list
 * offset: the offset of the click
 * first: the first (rendered) item
 * def: the default action
 */
static enum action
select_clicked(struct completions *cs, size_t offset, size_t first,
    enum action def)
{
	ssize_t selected = first;
	int set = 0;

	if (cs->length == 0)
		return NO_OP;

	if (offset < cs->completions[selected].offset)
		return EXIT;

	/* skip the first entry */
	for (selected += 1; selected < cs->length; ++selected) {
		if (cs->completions[selected].offset == -1)
			break;

		if (offset < cs->completions[selected].offset) {
			cs->selected = selected - 1;
			set = 1;
			break;
		}
	}

	if (!set)
		cs->selected = selected - 1;

	return def;
}

static enum action
handle_mouse(struct rendering *r, struct completions *cs,
    XButtonPressedEvent *e)
{
	size_t off;

	if (r->horizontal_layout)
		off = e->x;
	else
		off = e->y;

	switch (e->button) {
	case Button1:
		return select_clicked(cs, off, r->offset, CONFIRM);

	case Button3:
		return select_clicked(cs, off, r->offset, CONFIRM_CONTINUE);

	case Button4:
		return SCROLL_UP;

	case Button5:
		return SCROLL_DOWN;
	}

	return NO_OP;
}

/* event loop */
static enum state
loop(struct rendering *r, char **text, int *textlen, struct completions *cs,
    char **lines, char **vlines)
{
	enum action a;
	char *input = NULL;
	enum state status = LOOPING;
	int i;

	while (status == LOOPING) {
		XEvent e;
		XNextEvent(r->d, &e);

		if (XFilterEvent(&e, r->w))
			continue;

		switch (e.type) {
		case KeymapNotify:
			XRefreshKeyboardMapping(&e.xmapping);
			break;

		case FocusIn:
			/* Re-grab focus */
			if (e.xfocus.window != r->w)
				grabfocus(r->d, r->w);
			break;

		case VisibilityNotify:
			if (e.xvisibility.state != VisibilityUnobscured)
				XRaiseWindow(r->d, r->w);
			break;

		case MapNotify:
			get_wh(r->d, &r->w, &r->width, &r->height);
			draw(r, *text, cs);
			break;

		case KeyPress:
		case ButtonPress:
			if (e.type == KeyPress)
				a = parse_event(r->d, (XKeyPressedEvent *)&e,
				    r->xic, &input);
			else
				a = handle_mouse(r, cs,
				    (XButtonPressedEvent *)&e);

			switch (a) {
			case NO_OP:
				break;

			case EXIT:
				status = ERR;
				break;

			case CONFIRM:
				status = OK;
				confirm(&status, r, cs, text, textlen);
				break;

			case CONFIRM_CONTINUE:
				status = OK_LOOP;
				confirm(&status, r, cs, text, textlen);
				break;

			case PREV_COMPL:
				complete(cs, r->first_selected, 1, text,
				    textlen, &status);
				r->offset = cs->selected;
				break;

			case NEXT_COMPL:
				complete(cs, r->first_selected, 0, text,
				    textlen, &status);
				r->offset = cs->selected;
				break;

			case DEL_CHAR:
				popc(*text);
				update_completions(cs, *text, lines, vlines,
				    r->first_selected);
				r->offset = 0;
				break;

			case DEL_WORD:
				popw(*text);
				update_completions(cs, *text, lines, vlines,
				    r->first_selected);
				break;

			case DEL_LINE:
				for (i = 0; i < *textlen; ++i)
					(*text)[i] = 0;
				update_completions(cs, *text, lines, vlines,
				    r->first_selected);
				r->offset = 0;
				break;

			case ADD_CHAR:
				/*
				 * sometimes a strange key is pressed
				 * i.e. ctrl alone), so input will be
				 * empty. Don't need to update
				 * completion in that case
				 */
				if (*input == '\0')
					break;

				for (i = 0; input[i] != '\0'; ++i) {
					*textlen = pushc(text, *textlen,
					    input[i]);
					if (*textlen == -1) {
						fprintf(stderr,
						    "Memory allocation "
						    "error\n");
						status = ERR;
						break;
					}
				}

				if (status != ERR) {
					update_completions(cs, *text, lines,
					    vlines, r->first_selected);
					free(input);
				}

				r->offset = 0;
				break;

			case TOGGLE_FIRST_SELECTED:
				r->first_selected = !r->first_selected;
				if (r->first_selected && cs->selected < 0)
					cs->selected = 0;
				if (!r->first_selected && cs->selected == 0)
					cs->selected = -1;
				break;

			case SCROLL_DOWN:
				r->offset = MIN(r->offset + 1, cs->length - 1);
				break;

			case SCROLL_UP:
				r->offset = MAX((ssize_t)r->offset - 1, 0);
				break;
			}
		}

		draw(r, *text, cs);
	}

	return status;
}

static int
load_font(struct rendering *r, const char *fontname)
{
	r->font = XftFontOpenName(r->d, DefaultScreen(r->d), fontname);
	return 0;
}

static void
xim_init(struct rendering *r, XrmDatabase *xdb)
{
	XIMStyle best_match_style;
	XIMStyles *xis;
	int i;

	/* Open the X input method */
	if ((r->xim = XOpenIM(r->d, *xdb, RESNAME, RESCLASS)) == NULL)
		err(1, "XOpenIM");

	if (XGetIMValues(r->xim, XNQueryInputStyle, &xis, NULL) || !xis) {
		fprintf(stderr, "Input Styles could not be retrieved\n");
		exit(EX_UNAVAILABLE);
	}

	best_match_style = 0;
	for (i = 0; i < xis->count_styles; ++i) {
		XIMStyle ts = xis->supported_styles[i];
		if (ts == (XIMPreeditNothing | XIMStatusNothing)) {
			best_match_style = ts;
			break;
		}
	}
	XFree(xis);

	if (!best_match_style)
		fprintf(stderr,
		    "No matching input style could be determined\n");

	r->xic = XCreateIC(r->xim, XNInputStyle, best_match_style,
	    XNClientWindow, r->w, XNFocusWindow, r->w, NULL);
	if (r->xic == NULL)
		err(1, "XCreateIC");
}

static void
create_window(struct rendering *r, Window parent_window, Colormap cmap,
    XVisualInfo vinfo, int x, int y, int ox, int oy,
    unsigned long background_pixel)
{
	XSetWindowAttributes attr;
	unsigned long vmask;

	/* Create the window */
	attr.colormap = cmap;
	attr.override_redirect = 1;
	attr.border_pixel = 0;
	attr.background_pixel = background_pixel;
	attr.event_mask = StructureNotifyMask | ExposureMask | KeyPressMask
		| KeymapStateMask | ButtonPress | VisibilityChangeMask;

	vmask = CWBorderPixel | CWBackPixel | CWColormap | CWEventMask |
		CWOverrideRedirect;

	r->w = XCreateWindow(r->d, parent_window, x + ox, y + oy, r->width, r->height, 0,
	    vinfo.depth, InputOutput, vinfo.visual, vmask, &attr);
}

static void
ps1extents(struct rendering *r)
{
	char *dup;
	dup = strdupn(r->ps1);
	text_extents(dup == NULL ? r->ps1 : dup, r->ps1len, r, &r->ps1w, &r->ps1h);
	free(dup);
}

static void
usage(char *prgname)
{
	fprintf(stderr,
	    "%s [-Aahmv] [-B colors] [-b size] [-C color] [-c color]\n"
	    "       [-d separator] [-e window] [-f font] [-G color] [-g "
	    "size]\n"
	    "       [-H height] [-I color] [-i size] [-J color] [-j "
	    "size] [-l layout]\n"
	    "       [-P padding] [-p prompt] [-S color] [-s color] [-T "
	    "color]\n"
	    "       [-t color] [-W width] [-x coord] [-y coord]\n",
	    prgname);
}

int
main(int argc, char **argv)
{
	struct completions *cs;
	struct rendering r;
	XVisualInfo vinfo;
	Colormap cmap;
	size_t nlines, i;
	Window parent_window;
	XrmDatabase xdb;
	unsigned long fgs[3], bgs[3]; /* prompt, compl, compl_highlighted */
	unsigned long borders_bg[4], p_borders_bg[4], c_borders_bg[4],
		ch_borders_bg[4]; /* N E S W */
	enum state status = LOOPING;
	int ch;
	int offset_x = 0, offset_y = 0;
	int x = 0, y = 0;
	int textlen, d_width, d_height;
	short embed;
	const char *sep = NULL;
	const char *parent_window_id = NULL;
	char *tmp[4];
	char **lines, **vlines;
	char *fontname, *text, *xrm;

	setlocale(LC_ALL, getenv("LANG"));

	for (i = 0; i < 4; ++i) {
		/* default paddings */
		r.p_padding[i] = 10;
		r.c_padding[i] = 10;
		r.ch_padding[i] = 10;

		/* default borders */
		r.borders[i] = 0;
		r.p_borders[i] = 0;
		r.c_borders[i] = 0;
		r.ch_borders[i] = 0;
	}

	r.first_selected = 0;
	r.free_text = 1;
	r.multiple_select = 0;
	r.offset = 0;

	/* default width and height */
	r.width = 400;
	r.height = 20;

	/*
	 * The prompt. We duplicate the string so later is easy to
	 * free (in the case it's been overwritten by the user)
	 */
	if ((r.ps1 = strdup("$ ")) == NULL)
		err(1, "strdup");

	/* same for the font name */
	if ((fontname = strdup(DEFFONT)) == NULL)
		err(1, "strdup");

	while ((ch = getopt(argc, argv, ARGS)) != -1) {
		switch (ch) {
		case 'h': /* help */
			usage(*argv);
			return 0;
		case 'v': /* version */
			fprintf(stderr, "%s version: %s\n", *argv, VERSION);
			return 0;
		case 'e': /* embed */
			if ((parent_window_id = strdup(optarg)) == NULL)
				err(1, "strdup");
			break;
		case 'd':
			if ((sep = strdup(optarg)) == NULL)
				err(1, "strdup");
			break;
		case 'A':
			r.free_text = 0;
			break;
		case 'm':
			r.multiple_select = 1;
			break;
		default:
			break;
		}
	}

	lines = readlines(&nlines);

	vlines = NULL;
	if (sep != NULL) {
		int l;
		l = strlen(sep);
		if ((vlines = calloc(nlines, sizeof(char *))) == NULL)
			err(1, "calloc");

		for (i = 0; i < nlines; i++) {
			char *t;
			t = strstr(lines[i], sep);
			if (t == NULL)
				vlines[i] = lines[i];
			else
				vlines[i] = t + l;
		}
	}

	textlen = 10;
	if ((text = malloc(textlen * sizeof(char))) == NULL)
		err(1, "malloc");

	/* struct completions *cs = filter(text, lines); */
	if ((cs = compls_new(nlines)) == NULL)
		err(1, "compls_new");

	/* start talking to xorg */
	r.d = XOpenDisplay(NULL);
	if (r.d == NULL) {
		fprintf(stderr, "Could not open display!\n");
		return EX_UNAVAILABLE;
	}

	embed = 1;
	if (!(parent_window_id && (parent_window = strtol(parent_window_id, NULL, 0)))) {
		parent_window = DefaultRootWindow(r.d);
		embed = 0;
	}

	/* get display size */
	get_wh(r.d, &parent_window, &d_width, &d_height);

	if (!embed)
		findmonitor(r.d, &offset_x, &offset_y, &d_width, &d_height);

	XMatchVisualInfo(r.d, DefaultScreen(r.d), 32, TrueColor, &vinfo);
	cmap = XCreateColormap(r.d, XDefaultRootWindow(r.d), vinfo.visual,
	    AllocNone);

	fgs[0] = fgs[1] = parse_color("#fff", NULL);
	fgs[2] = parse_color("#000", NULL);

	bgs[0] = bgs[1] = parse_color("#000", NULL);
	bgs[2] = parse_color("#fff", NULL);

	borders_bg[0] = borders_bg[1] = borders_bg[2] = borders_bg[3] =
		parse_color("#000", NULL);

	p_borders_bg[0] = p_borders_bg[1] = p_borders_bg[2] = p_borders_bg[3]
		= parse_color("#000", NULL);
	c_borders_bg[0] = c_borders_bg[1] = c_borders_bg[2] = c_borders_bg[3]
		= parse_color("#000", NULL);
	ch_borders_bg[0] = ch_borders_bg[1] = ch_borders_bg[2] = ch_borders_bg[3]
		= parse_color("#000", NULL);

	r.horizontal_layout = 1;

	/* Read the resources */
	XrmInitialize();
	xrm = XResourceManagerString(r.d);
	xdb = NULL;
	if (xrm != NULL) {
		XrmValue value;
		char *datatype[20];

		xdb = XrmGetStringDatabase(xrm);

		if (XrmGetResource(xdb, "MyMenu.font", "*", datatype, &value)) {
			free(fontname);
			if ((fontname = strdup(value.addr)) == NULL)
				err(1, "strdup");
		} else {
			fprintf(stderr, "no font defined, using %s\n", fontname);
		}

		if (XrmGetResource(xdb, "MyMenu.layout", "*", datatype, &value))
			r.horizontal_layout = !strcmp(value.addr, "horizontal");
		else
			fprintf(stderr, "no layout defined, using horizontal\n");

		if (XrmGetResource(xdb, "MyMenu.prompt", "*", datatype, &value)) {
			free(r.ps1);
			r.ps1 = normalize_str(value.addr);
		} else {
			fprintf(stderr,
				"no prompt defined, using \"%s\" as "
				"default\n",
				r.ps1);
		}

		if (XrmGetResource(xdb, "MyMenu.prompt.border.size", "*", datatype, &value)) {
			if (parse_csslike(value.addr, tmp) == -1)
				err(1, "parse_csslike");
			for (i = 0; i < 4; ++i) {
				r.p_borders[i] = parse_integer(tmp[i], 0);
				free(tmp[i]);
			}
		}

		if (XrmGetResource(xdb, "MyMenu.prompt.border.color", "*", datatype, &value)) {
			if (parse_csslike(value.addr, tmp) == -1)
				err(1, "parse_csslike");

			for (i = 0; i < 4; ++i) {
				p_borders_bg[i] = parse_color(tmp[i], "#000");
				free(tmp[i]);
			}
		}

		if (XrmGetResource(xdb, "MyMenu.prompt.padding", "*", datatype, &value)) {
			if (parse_csslike(value.addr, tmp) == -1)
				err(1, "parse_csslike");

			for (i = 0; i < 4; ++i) {
				r.p_padding[i] = parse_integer(tmp[i], 0);
				free(tmp[i]);
			}
		}

		if (XrmGetResource(xdb, "MyMenu.width", "*", datatype, &value))
			r.width = parse_int_with_percentage(value.addr, r.width, d_width);
		else
			fprintf(stderr, "no width defined, using %d\n", r.width);

		if (XrmGetResource(xdb, "MyMenu.height", "*", datatype, &value))
			r.height = parse_int_with_percentage(value.addr, r.height, d_height);
		else
			fprintf(stderr, "no height defined, using %d\n", r.height);

		if (XrmGetResource(xdb, "MyMenu.x", "*", datatype, &value))
			x = parse_int_with_pos(r.d, value.addr, x, d_width, r.width);

		if (XrmGetResource(xdb, "MyMenu.y", "*", datatype, &value))
			y = parse_int_with_pos(r.d, value.addr, y, d_height, r.height);

		if (XrmGetResource(xdb, "MyMenu.border.size", "*", datatype, &value)) {
			if (parse_csslike(value.addr, tmp) == -1)
				err(1, "parse_csslike");

			for (i = 0; i < 4; ++i) {
				r.borders[i] = parse_int_with_percentage(tmp[i], 0,
				    (i % 2) == 0 ? d_height : d_width);
				free(tmp[i]);
			}
		}

		/* Prompt */
		if (XrmGetResource(xdb, "MyMenu.prompt.foreground", "*", datatype, &value))
			fgs[0] = parse_color(value.addr, "#fff");

		if (XrmGetResource(xdb, "MyMenu.prompt.background", "*", datatype, &value))
			bgs[0] = parse_color(value.addr, "#000");

		/* Completions */
		if (XrmGetResource(xdb, "MyMenu.completion.foreground", "*", datatype, &value))
			fgs[1] = parse_color(value.addr, "#fff");

		if (XrmGetResource(xdb, "MyMenu.completion.background", "*", datatype, &value))
			bgs[1] = parse_color(value.addr, "#000");

		if (XrmGetResource(xdb, "MyMenu.completion.padding", "*", datatype, &value)) {
			if (parse_csslike(value.addr, tmp) == -1)
				err(1, "parse_csslike");

			for (i = 0; i < 4; ++i) {
				r.c_padding[i] = parse_integer(tmp[i], 0);
				free(tmp[i]);
			}
		}

		if (XrmGetResource(xdb, "MyMenu.completion.border.size", "*", datatype, &value)) {
			if (parse_csslike(value.addr, tmp) == -1)
				err(1, "parse_csslike");

			for (i = 0; i < 4; ++i) {
				r.c_borders[i] = parse_integer(tmp[i], 0);
				free(tmp[i]);
			}
		}

		if (XrmGetResource(xdb, "MyMenu.completion.border.color", "*", datatype, &value)) {
			if (parse_csslike(value.addr, tmp) == -1)
				err(1, "parse_csslike");

			for (i = 0; i < 4; ++i) {
				c_borders_bg[i] = parse_color(tmp[i], "#000");
				free(tmp[i]);
			}
		}

		/* Completion Highlighted */
		if (XrmGetResource(
			    xdb, "MyMenu.completion_highlighted.foreground", "*", datatype, &value))
			fgs[2] = parse_color(value.addr, "#000");

		if (XrmGetResource(
			    xdb, "MyMenu.completion_highlighted.background", "*", datatype, &value))
			bgs[2] = parse_color(value.addr, "#fff");

		if (XrmGetResource(
			    xdb, "MyMenu.completion_highlighted.padding", "*", datatype, &value)) {
			if (parse_csslike(value.addr, tmp) == -1)
				err(1, "parse_csslike");

			for (i = 0; i < 4; ++i) {
				r.ch_padding[i] = parse_integer(tmp[i], 0);
				free(tmp[i]);
			}
		}

		if (XrmGetResource(xdb, "MyMenu.completion_highlighted.border.size", "*", datatype,
		    &value)) {
			if (parse_csslike(value.addr, tmp) == -1)
				err(1, "parse_csslike");

			for (i = 0; i < 4; ++i) {
				r.ch_borders[i] = parse_integer(tmp[i], 0);
				free(tmp[i]);
			}
		}

		if (XrmGetResource(xdb, "MyMenu.completion_highlighted.border.color", "*", datatype,
			    &value)) {
			if (parse_csslike(value.addr, tmp) == -1)
				err(1, "parse_csslike");

			for (i = 0; i < 4; ++i) {
				ch_borders_bg[i] = parse_color(tmp[i], "#000");
				free(tmp[i]);
			}
		}

		/* Border */
		if (XrmGetResource(xdb, "MyMenu.border.color", "*", datatype, &value)) {
			if (parse_csslike(value.addr, tmp) == -1)
				err(1, "parse_csslike");

			for (i = 0; i < 4; ++i) {
				borders_bg[i] = parse_color(tmp[i], "#000");
				free(tmp[i]);
			}
		}
	}

	/* Second round of args parsing */
	optind = 0; /* reset the option index */
	while ((ch = getopt(argc, argv, ARGS)) != -1) {
		switch (ch) {
		case 'a':
			r.first_selected = 1;
			break;
		case 'A':
			/* free_text -- already catched */
		case 'd':
			/* separator -- this case was already catched */
		case 'e':
			/* embedding mymenu this case was already catched. */
		case 'm':
			/* multiple selection this case was already catched.
			 */
			break;
		case 'p': {
			char *newprompt;
			newprompt = strdup(optarg);
			if (newprompt != NULL) {
				free(r.ps1);
				r.ps1 = newprompt;
			}
			break;
		}
		case 'x':
			x = parse_int_with_pos(r.d, optarg, x, d_width, r.width);
			break;
		case 'y':
			y = parse_int_with_pos(r.d, optarg, y, d_height, r.height);
			break;
		case 'P':
			if (parse_csslike(optarg, tmp) == -1)
				err(1, "parse_csslike");
			for (i = 0; i < 4; ++i)
				r.p_padding[i] = parse_integer(tmp[i], 0);
			break;
		case 'G':
			if (parse_csslike(optarg, tmp) == -1)
				err(1, "parse_csslike");
			for (i = 0; i < 4; ++i)
				p_borders_bg[i] = parse_color(tmp[i], "#000");
			break;
		case 'g':
			if (parse_csslike(optarg, tmp) == -1)
				err(1, "parse_csslike");
			for (i = 0; i < 4; ++i)
				r.p_borders[i] = parse_integer(tmp[i], 0);
			break;
		case 'I':
			if (parse_csslike(optarg, tmp) == -1)
				err(1, "parse_csslike");
			for (i = 0; i < 4; ++i)
				c_borders_bg[i] = parse_color(tmp[i], "#000");
			break;
		case 'i':
			if (parse_csslike(optarg, tmp) == -1)
				err(1, "parse_csslike");
			for (i = 0; i < 4; ++i)
				r.c_borders[i] = parse_integer(tmp[i], 0);
			break;
		case 'J':
			if (parse_csslike(optarg, tmp) == -1)
				err(1, "parse_csslike");
			for (i = 0; i < 4; ++i)
				ch_borders_bg[i] = parse_color(tmp[i], "#000");
			break;
		case 'j':
			if (parse_csslike(optarg, tmp) == -1)
				err(1, "parse_csslike");
			for (i = 0; i < 4; ++i)
				r.ch_borders[i] = parse_integer(tmp[i], 0);
			break;
		case 'l':
			r.horizontal_layout = !strcmp(optarg, "horizontal");
			break;
		case 'f': {
			char *newfont;
			if ((newfont = strdup(optarg)) != NULL) {
				free(fontname);
				fontname = newfont;
			}
			break;
		}
		case 'W':
			r.width = parse_int_with_percentage(optarg, r.width, d_width);
			break;
		case 'H':
			r.height = parse_int_with_percentage(optarg, r.height, d_height);
			break;
		case 'b':
			if (parse_csslike(optarg, tmp) == -1)
				err(1, "parse_csslike");
			for (i = 0; i < 4; ++i)
				r.borders[i] = parse_integer(tmp[i], 0);
			break;
		case 'B':
			if (parse_csslike(optarg, tmp) == -1)
				err(1, "parse_csslike");
			for (i = 0; i < 4; ++i)
				borders_bg[i] = parse_color(tmp[i], "#000");
			break;
		case 't':
			fgs[0] = parse_color(optarg, NULL);
			break;
		case 'T':
			bgs[0] = parse_color(optarg, NULL);
			break;
		case 'c':
			fgs[1] = parse_color(optarg, NULL);
			break;
		case 'C':
			bgs[1] = parse_color(optarg, NULL);
			break;
		case 's':
			fgs[2] = parse_color(optarg, NULL);
			break;
		case 'S':
			bgs[2] = parse_color(optarg, NULL);
			break;
		default:
			fprintf(stderr, "Unrecognized option %c\n", ch);
			status = ERR;
			break;
		}
	}

	if (r.height < 0 || r.width < 0 || x < 0 || y < 0) {
		fprintf(stderr, "height, width, x or y are lesser than 0.");
		status = ERR;
	}

	/* since only now we know if the first should be selected,
	 * update the completion here */
	update_completions(cs, text, lines, vlines, r.first_selected);

	/* update the prompt lenght, only now we surely know the length of it
	 */
	r.ps1len = strlen(r.ps1);

	/* Create the window */
	create_window(&r, parent_window, cmap, vinfo, x, y, offset_x, offset_y, bgs[1]);
	set_win_atoms_hints(r.d, r.w, r.width, r.height);
	XMapRaised(r.d, r.w);

	/* If embed, listen for other events as well */
	if (embed) {
		Window *children, parent, root;
		unsigned int children_no;

		XSelectInput(r.d, parent_window, FocusChangeMask);
		if (XQueryTree(r.d, parent_window, &root, &parent, &children, &children_no)
			&& children) {
			for (i = 0; i < children_no && children[i] != r.w; ++i)
				XSelectInput(r.d, children[i], FocusChangeMask);
			XFree(children);
		}
		grabfocus(r.d, r.w);
	}

	take_keyboard(r.d, r.w);

	r.x_zero = r.borders[3];
	r.y_zero = r.borders[0];

	{
		XGCValues values;

		for (i = 0; i < 3; ++i) {
			r.fgs[i] = XCreateGC(r.d, r.w, 0, &values);
			r.bgs[i] = XCreateGC(r.d, r.w, 0, &values);
		}

		for (i = 0; i < 4; ++i) {
			r.borders_bg[i] = XCreateGC(r.d, r.w, 0, &values);
			r.p_borders_bg[i] = XCreateGC(r.d, r.w, 0, &values);
			r.c_borders_bg[i] = XCreateGC(r.d, r.w, 0, &values);
			r.ch_borders_bg[i] = XCreateGC(r.d, r.w, 0, &values);
		}
	}

	/* Load the colors in our GCs */
	for (i = 0; i < 3; ++i) {
		XSetForeground(r.d, r.fgs[i], fgs[i]);
		XSetForeground(r.d, r.bgs[i], bgs[i]);
	}

	for (i = 0; i < 4; ++i) {
		XSetForeground(r.d, r.borders_bg[i], borders_bg[i]);
		XSetForeground(r.d, r.p_borders_bg[i], p_borders_bg[i]);
		XSetForeground(r.d, r.c_borders_bg[i], c_borders_bg[i]);
		XSetForeground(r.d, r.ch_borders_bg[i], ch_borders_bg[i]);
	}

	if (load_font(&r, fontname) == -1)
		status = ERR;

	r.xftdraw = XftDrawCreate(r.d, r.w, vinfo.visual, cmap);

	for (i = 0; i < 3; ++i) {
		rgba_t c;
		XRenderColor xrcolor;

		c = *(rgba_t *)&fgs[i];
		xrcolor.red = EXPANDBITS(c.rgba.r);
		xrcolor.green = EXPANDBITS(c.rgba.g);
		xrcolor.blue = EXPANDBITS(c.rgba.b);
		xrcolor.alpha = EXPANDBITS(c.rgba.a);
		XftColorAllocValue(r.d, vinfo.visual, cmap, &xrcolor, &r.xft_colors[i]);
	}

	/* compute prompt dimensions */
	ps1extents(&r);

	xim_init(&r, &xdb);

#ifdef __OpenBSD__
	if (pledge("stdio", "") == -1)
		err(1, "pledge");
#endif

	/* Cache text height */
	text_extents("fyjpgl", 6, &r, NULL, &r.text_height);

	/* Draw the window for the first time */
	draw(&r, text, cs);

	/* Main loop */
	while (status == LOOPING || status == OK_LOOP) {
		status = loop(&r, &text, &textlen, cs, lines, vlines);

		if (status != ERR)
			printf("%s\n", text);

		if (!r.multiple_select && status == OK_LOOP)
			status = OK;
	}

	XUngrabKeyboard(r.d, CurrentTime);

	for (i = 0; i < 3; ++i)
		XftColorFree(r.d, vinfo.visual, cmap, &r.xft_colors[i]);

	for (i = 0; i < 3; ++i) {
		XFreeGC(r.d, r.fgs[i]);
		XFreeGC(r.d, r.bgs[i]);
	}

	for (i = 0; i < 4; ++i) {
		XFreeGC(r.d, r.borders_bg[i]);
		XFreeGC(r.d, r.p_borders_bg[i]);
		XFreeGC(r.d, r.c_borders_bg[i]);
		XFreeGC(r.d, r.ch_borders_bg[i]);
	}

	XDestroyIC(r.xic);
	XCloseIM(r.xim);

	for (i = 0; i < 3; ++i)
		XftColorFree(r.d, vinfo.visual, cmap, &r.xft_colors[i]);
	XftFontClose(r.d, r.font);
	XftDrawDestroy(r.xftdraw);

	free(r.ps1);
	free(fontname);
	free(text);

	free(lines);
	free(vlines);
	compls_delete(cs);

	XFreeColormap(r.d, cmap);

	XDestroyWindow(r.d, r.w);
	XCloseDisplay(r.d);

	return status != OK;
}
