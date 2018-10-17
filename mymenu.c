#include <stdio.h>
#include <stdlib.h>
#include <string.h>		/* strdup, strlen */
#include <ctype.h>		/* isalnum */
#include <locale.h>		/* setlocale */
#include <unistd.h>
#include <sysexits.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>
#include <X11/Xcms.h>
#include <X11/keysym.h>

#ifdef USE_XINERAMA
# include <X11/extensions/Xinerama.h>
#endif

#ifdef USE_XFT
# include <X11/Xft/Xft.h>
#endif

#ifndef VERSION
# define VERSION "unknown"
#endif

#define resname "MyMenu"
#define resclass "mymenu"

#define SYM_BUF_SIZE 4

#ifdef USE_XFT
# define default_fontname "monospace"
#else
# define default_fontname "fixed"
#endif

#define ARGS "Aahmve:p:P:l:f:W:H:x:y:b:B:t:T:c:C:s:S:d:G:g:I:i:J:j:"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define EXPANDBITS(x) (((x & 0xf0) * 0x100) | (x & 0x0f) * 0x10)

/*
 * If we don't have or we don't want an "ignore case" completion
 * style, fall back to `strstr(3)`
 */
#ifndef USE_STRCASESTR
# define strcasestr strstr
#endif

/* The number of char to read */
#define STDIN_CHUNKS 128

/* The number of lines to allocate in advance */
#define LINES_CHUNK 64

/* Abort on NULL */
#define check_allocation(a) {				\
    if (a == NULL) {					\
      fprintf(stderr, "Could not allocate memory\n");	\
      abort();						\
    }							\
  }

#define inner_height(r) (r->height - r->borders[0] - r->borders[2])
#define inner_width(r)  (r->width  - r->borders[1] - r->borders[3])

/* The states of the event loop */
enum state {LOOPING, OK_LOOP, OK, ERR};

/* 
 * For the drawing-related function. The text to be rendere could be
 * the prompt, a completion or a highlighted completion
 */
enum obj_type {PROMPT, COMPL, COMPL_HIGH};

/* These are the possible action to be performed after user input. */
enum action {
	EXIT,
	CONFIRM,
	CONFIRM_CONTINUE,
	NEXT_COMPL,
	PREV_COMPL,
	DEL_CHAR,
	DEL_WORD,
	DEL_LINE,
	ADD_CHAR,
	TOGGLE_FIRST_SELECTED
};

/* A big set of values that needs to be carried around for drawing. A
big struct to rule them all */
struct rendering {
	Display		*d;	/* Connection to xorg */
	Window		w;
	int		width;
	int		height;
	int		p_padding[4];
	int		c_padding[4];
	int		ch_padding[4];
	int		x_zero; /* the "zero" on the x axis (may not be exactly 0 'cause the borders) */
	int		y_zero; /* like x_zero but for the y axis */

	size_t		offset; /* scroll offset */

	short		free_text;
	short		first_selected;
	short		multiple_select;

	/* four border width */
	int		borders[4];
	int		p_borders[4];
	int		c_borders[4];
	int		ch_borders[4];

	short		horizontal_layout;

	/* prompt */
	char		*ps1;
	int		ps1len;
	int		ps1w;	/* ps1 width */
	int		ps1h;	/* ps1 height */

	int		text_height; /* cache for the vertical layout */

	XIC		xic;

	/* colors */
	GC		fgs[4];
	GC		bgs[4];
	GC		borders_bg[4];
	GC		p_borders_bg[4];
	GC		c_borders_bg[4];
	GC		ch_borders_bg[4];
#ifdef USE_XFT
	XftFont		*font;
	XftDraw		*xftdraw;
	XftColor	xft_colors[3];
#else
	XFontSet	font;
#endif
};

struct completion {
	char	*completion;
	char	*rcompletion;
};

/* Wrap the linked list of completions */
struct completions {
	struct completion	*completions;
	ssize_t			selected;
	size_t			length;
};

/* idea stolen from lemonbar. ty lemonboy */
typedef union {
	struct {
		uint8_t	b;
		uint8_t	g;
		uint8_t	r;
		uint8_t	a;
	} rgba;
	uint32_t	v;
} rgba_t;

extern char	*optarg;
extern int	optind;

/* Return a newly allocated (and empty) completion list */
struct completions *
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
void
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
void
filter(struct completions *cs, char *text, char **lines, char **vlines)
{
	size_t	index = 0;
	size_t	matching = 0;
	char	*l;

	if (vlines == NULL)
		vlines = lines;

	while (1) {
		if (lines[index] == NULL)
			break;

		l = vlines[index] != NULL ? vlines[index] : lines[index];

		if (strcasestr(l, text) != NULL) {
			struct completion *c = &cs->completions[matching];
			c->completion  = l;
			c->rcompletion = lines[index];
			matching++;
		}

		index++;
	}
	cs->length = matching;
	cs->selected = -1;
}

/* Update the given completion */
void
update_completions(struct completions *cs, char *text, char **lines, char **vlines, short first_selected)
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
void
complete(struct completions *cs, short first_selected, short p, char **text, int *textlen, enum state *status)
{
	struct completion	*n;
	int			index;

	if (cs == NULL || cs->length == 0)
		return;

	/*
	 * If the first is always selected and the first entry is
	 * different from the text, expand the text and return
	 */
	if (first_selected
	    && cs->selected == 0
	    && strcmp(cs->completions->completion, *text) != 0
	    && !p) {
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
	index = cs->selected = (cs->length + (p ? index - 1 : index + 1)) % cs->length;

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
int
pushc(char **p, int maxlen, char c)
{
	int	len;

	len = strnlen(*p, maxlen);
	if (!(len < maxlen -2)) {
		char *newptr;

		maxlen += maxlen >> 1;
		newptr = realloc(*p, maxlen);
		if (newptr == NULL) /* bad */
			return -1;
		*p = newptr;
	}

	(*p)[len] = c;
	(*p)[len+1] = '\0';
	return maxlen;
}

/* 
 * Remove the last rune from the *UTF-8* string! This is different
 * from just setting the last byte to 0 (in some cases ofc). Return a
 * pointer (e) to the last nonzero char. If e < p then p is empty!
 */
char *
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
void
popw(char *w)
{
	int	len;
	short	in_word = 1;

	if ((len = strlen(w)) == 0)
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
char *
normalize_str(const char *str)
{
	int	len, p;
	char	*s;

	if ((len = strlen(str)) == 0)
		return NULL;

	s = calloc(len, sizeof(char));
	check_allocation(s);
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
			str++;	/* skip only this char */
			continue;
		}
		s[p] = c;
		p++;
		str++;
	}

	return s;
}

size_t
read_stdin(char **buf)
{
	size_t	offset = 0;
	size_t	len = STDIN_CHUNKS;

	*buf = malloc(len * sizeof(char));
	if (*buf == NULL)
		goto err;

	while (1) {
		ssize_t r;
		size_t i;

		r = read(0, *buf + offset, STDIN_CHUNKS);

		if (r < 1)
			return len;

		offset += r;

		len += STDIN_CHUNKS;
		*buf = realloc(*buf, len);
		if (*buf == NULL)
			goto err;

		for (i = offset; i < len; ++i)
			(*buf)[i] = '\0';
	}

 err:
	fprintf(stderr, "Error in allocating memory for stdin.\n");
	exit(EX_UNAVAILABLE);
}

size_t
readlines(char ***lns, char **buf)
{
	size_t	i, len, ll, lines;
	short	in_line = 0;

	lines = 0;

	*buf = NULL;
	len = read_stdin(buf);

	ll = LINES_CHUNK;
	*lns = malloc(ll * sizeof(char*));

	if (*lns == NULL)
		goto err;

	for (i = 0; i < len; i++) {
		char c = (*buf)[i];

		if (c == '\0')
			break;

		if (c == '\n')
			(*buf)[i] = '\0';

		if (in_line && c == '\n')
			in_line = 0;

		if (!in_line && c != '\n') {
			in_line = 1;
			(*lns)[lines] = (*buf) + i;
			lines++;

			if (lines == ll) { /* resize */
				ll += LINES_CHUNK;
				*lns = realloc(*lns, ll * sizeof(char*));
				if (*lns == NULL)
					goto err;
			}
		}
	}

	(*lns)[lines] = NULL;

	return lines;

 err:
	fprintf(stderr, "Error in memory allocation.\n");
	exit(EX_UNAVAILABLE);
}

/*
 * Compute the dimensions of the string str once rendered.
 * It'll return the width and set ret_width and ret_height if not NULL
 */
int
text_extents(char *str, int len, struct rendering *r, int *ret_width, int *ret_height)
{
	int	height;
	int	width;
#ifdef USE_XFT
	XGlyphInfo	gi;
	XftTextExtentsUtf8(r->d, r->font, str, len, &gi);
	height = r->font->ascent - r->font->descent;
	width = gi.width - gi.x;
#else
	XRectangle	rect;
	XmbTextExtents(r->font, str, len, NULL, &rect);
	height = rect.height;
	width = rect.width;
#endif
	if (ret_width != NULL)  *ret_width = width;
	if (ret_height != NULL) *ret_height = height;
	return width;
}

void
draw_string(char *str, int len, int x, int y, struct rendering *r, enum obj_type tt)
{
#ifdef USE_XFT
	XftColor xftcolor;
	if (tt == PROMPT)     xftcolor = r->xft_colors[0];
	if (tt == COMPL)      xftcolor = r->xft_colors[1];
	if (tt == COMPL_HIGH) xftcolor = r->xft_colors[2];

	XftDrawStringUtf8(r->xftdraw, &xftcolor, r->font, x, y, str, len);
#else
	GC gc;
	if (tt == PROMPT)     gc = r->fgs[0];
	if (tt == COMPL)      gc = r->fgs[1];
	if (tt == COMPL_HIGH) gc = r->fgs[2];
	Xutf8DrawString(r->d, r->w, r->font, gc, x, y, str, len);
#endif
}

/* Duplicate the string and substitute every space with a 'n` */
char *
strdupn(char *str)
{
	int	len, i;
	char	*dup;

	len = strlen(str);

	if (str == NULL || len == 0)
		return NULL;

	if ((dup = strdup(str)) == NULL)
		return NULL;

	for (i = 0; i < len; ++i)
		if (dup[i] == ' ')
			dup[i] = 'n';

	return dup;
}

int
draw_v_box(struct rendering *r, int y, char *prefix, int prefix_width, enum obj_type t, char *text)
{
	GC	*border_color, bg;
	int	*padding, *borders;
	int	ret = 0, inner_width, inner_height, x;

	switch (t) {
	case PROMPT:
		border_color	= r->p_borders_bg;
		padding		= r->p_padding;
		borders		= r->p_borders;
		bg		= r->bgs[0];
		break;
	case COMPL:
		border_color	= r->c_borders_bg;
		padding		= r->c_padding;
		borders		= r->c_borders;
		bg		= r->bgs[1];
		break;
	case COMPL_HIGH:
		border_color	= r->ch_borders_bg;
		padding		= r->ch_padding;
		borders		= r->ch_borders;
		bg		= r->bgs[2];
		break;
	}

	ret = borders[0] + padding[0] + r->text_height + padding[2] + borders[2];

	inner_width = inner_width(r) - borders[1] - borders[3];
	inner_height = padding[0] + r->text_height + padding[2];

	/* Border top */
	XFillRectangle(r->d, r->w, border_color[0], r->x_zero, y, r->width, borders[0]);

	/* Border right */
	XFillRectangle(r->d, r->w, border_color[1], r->x_zero + inner_width(r) - borders[1] , y, borders[1], ret);

	/* Border bottom */
	XFillRectangle(r->d, r->w, border_color[2], r->x_zero, y + borders[0] + padding[0] + r->text_height + padding[2], r->width, borders[2]);

	/* Border left */
	XFillRectangle(r->d, r->w, border_color[3], r->x_zero, y, borders[3], ret);

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

int
draw_h_box(struct rendering *r, int x, char *prefix, int prefix_width, enum obj_type t, char *text)
{
	GC	*border_color, bg;
	int	*padding, *borders;
	int	ret = 0, inner_width, inner_height, y, text_width;

	switch (t) {
	case PROMPT:
		border_color	= r->p_borders_bg;
		padding		= r->p_padding;
		borders		= r->p_borders;
		bg		= r->bgs[0];
		break;
	case COMPL:
		border_color	= r->c_borders_bg;
		padding		= r->c_padding;
		borders		= r->c_borders;
		bg		= r->bgs[1];
		break;
	case COMPL_HIGH:
		border_color	= r->ch_borders_bg;
		padding		= r->ch_padding;
		borders		= r->ch_borders;
		bg		= r->bgs[2];
		break;
	}

	if (padding[0] < 0 || padding[2] < 0)
		padding[0] = padding[2] = (inner_height(r) - borders[0] - borders[2] - r->text_height)/2;

	/* If they are still lesser than 0, set 'em to 0 */
	if (padding[0] < 0 || padding[2] < 0)
		padding[0] = padding[2] = 0;

	/* Get the text width */
	text_extents(text, strlen(text), r, &text_width, NULL);
	if (prefix != NULL)
		text_width += prefix_width;

	ret = borders[3] + padding[3] + text_width + padding[1] + borders[1];

	inner_width = padding[3] + text_width + padding[1];
	inner_height = inner_height(r) - borders[0] - borders[2];

	/* Border top */
	XFillRectangle(r->d, r->w, border_color[0], x, r->y_zero, ret, borders[0]);

	/* Border right */
	XFillRectangle(r->d, r->w, border_color[1], x + borders[3] + inner_width, r->y_zero, borders[1], inner_height(r));

	/* Border bottom */
	XFillRectangle(r->d, r->w, border_color[2], x, r->y_zero + inner_height(r) - borders[2], ret, borders[2]);

	/* Border left */
	XFillRectangle(r->d, r->w, border_color[3], x, r->y_zero, borders[3], inner_height(r));

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

/* ,-----------------------------------------------------------------, */
/* | 20 char text     | completion | completion | completion | compl | */
/* `-----------------------------------------------------------------' */
void
draw_horizontally(struct rendering *r, char *text, struct completions *cs)
{
	size_t	i;
	int	x = r->x_zero;

	/* Draw the prompt */
	x += draw_h_box(r, x, r->ps1, r->ps1w, PROMPT, text);

	for (i = r->offset; i < cs->length; ++i) {
		enum obj_type t = cs->selected == (ssize_t)i ? COMPL_HIGH : COMPL;

		x += draw_h_box(r, x, NULL, 0, t, cs->completions[i].completion);

		if (x > inner_width(r))
			break;
	}
}

/* ,-----------------------------------------------------------------, */
/* |  prompt                                                         | */
/* |-----------------------------------------------------------------| */
/* |  completion                                                     | */
/* |-----------------------------------------------------------------| */
/* |  completion                                                     | */
/* `-----------------------------------------------------------------' */
void
draw_vertically(struct rendering *r, char *text, struct completions *cs)
{
	size_t	i;
	int	y = r->y_zero;

	y += draw_v_box(r, y, r->ps1, r->ps1w, PROMPT, text);

	for (i = r->offset; i < cs->length; ++i) {
		enum obj_type t = cs->selected == (ssize_t)i ? COMPL_HIGH : COMPL;

		y += draw_v_box(r, y, NULL, 0, t, cs->completions[i].completion);

		if (y > inner_height(r))
			break;
	}
}

void
draw(struct rendering *r, char *text, struct completions *cs)
{
	/* Draw the background */
	XFillRectangle(r->d, r->w, r->bgs[1], r->x_zero, r->y_zero, inner_width(r), inner_height(r));

	/* Draw the contents */
	if (r->horizontal_layout)
		draw_horizontally(r, text, cs);
	else
		draw_vertically(r, text, cs);

	/* Draw the borders */
	if (r->borders[0] != 0)
		XFillRectangle(r->d, r->w, r->borders_bg[0], 0, 0, r->width, r->borders[0]);

	if (r->borders[1] != 0)
		XFillRectangle(r->d, r->w, r->borders_bg[1], r->width - r->borders[1], 0, r->borders[1], r->height);

	if (r->borders[2] != 0)
		XFillRectangle(r->d, r->w, r->borders_bg[2], 0, r->height - r->borders[2], r->width, r->borders[2]);

	if (r->borders[3] != 0)
		XFillRectangle(r->d, r->w, r->borders_bg[3], 0, 0, r->borders[3], r->height);

	/* render! */
	XFlush(r->d);
}

/* Set some WM stuff */
void
set_win_atoms_hints(Display *d, Window w, int width, int height)
{
	Atom		type;
	XClassHint	*class_hint;
	XSizeHints	*size_hint;

	type = XInternAtom(d, "_NET_WM_WINDOW_TYPE_DOCK", 0);
	XChangeProperty(d,
			w,
			XInternAtom(d, "_NET_WM_WINDOW_TYPE", 0),
			XInternAtom(d, "ATOM", 0),
			32,
			PropModeReplace,
			(unsigned char *)&type,
			1
			);

	/* some window managers honor this properties */
	type = XInternAtom(d, "_NET_WM_STATE_ABOVE", 0);
	XChangeProperty(d,
			w,
			XInternAtom(d, "_NET_WM_STATE", 0),
			XInternAtom(d, "ATOM", 0),
			32,
			PropModeReplace,
			(unsigned char *)&type,
			1
			);

	type = XInternAtom(d, "_NET_WM_STATE_FOCUSED", 0);
	XChangeProperty(d,
			w,
			XInternAtom(d, "_NET_WM_STATE", 0),
			XInternAtom(d, "ATOM", 0),
			32,
			PropModeAppend,
			(unsigned char *)&type,
			1
			);

	/* Setting window hints */
	class_hint = XAllocClassHint();
	if (class_hint == NULL) {
		fprintf(stderr, "Could not allocate memory for class hint\n");
		exit(EX_UNAVAILABLE);
	}

	class_hint->res_name = resname;
	class_hint->res_class = resclass;
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
void
get_wh(Display *d, Window *w, int *width, int *height)
{
	XWindowAttributes win_attr;

	XGetWindowAttributes(d, *w, &win_attr);
	*height = win_attr.height;
	*width = win_attr.width;
}

int
grabfocus(Display *d, Window w)
{
	int	i;
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
int
take_keyboard(Display *d, Window w)
{
	int i;
	for (i = 0; i < 100; i++) {
		if (XGrabKeyboard(d, w, 1, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess)
			return 1;
		usleep(1000);
	}
	fprintf(stderr, "Cannot grab keyboard\n");
	return 0;
}

unsigned long
parse_color(const char *str, const char *def)
{
	size_t len;
	rgba_t tmp;
	char *ep;

	if (str == NULL)
		goto invc;

	len = strlen(str);

	/* +1 for the # ath the start */
	if (*str != '#' || len > 9 || len < 4)
		goto invc;
	++str; 			/* skip the # */

	errno = 0;
	tmp = (rgba_t)(uint32_t)strtoul(str, &ep, 16);

	if (errno)
		goto invc;

	switch (len-1) {
	case 3:
		/* expand #rgb -> #rrggbb */
		tmp.v =   (tmp.v & 0xf00) * 0x1100
			| (tmp.v & 0x0f0) * 0x0110
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

 invc:
	fprintf(stderr, "Invalid color: \"%s\".\n", str);
	if (def != NULL)
		return parse_color(def, NULL);
	else
		return 0U;
}

/* 
 * Given a string try to parse it as a number or return `default_value'.
 */
int
parse_integer(const char *str, int default_value)
{
	long	lval;
	char	*ep;

	errno = 0;
	lval = strtol(str, &ep, 10);

	if (str[0] == '\0' || *ep != '\0') { /* NaN */
		fprintf(stderr, "'%s' is not a valid number! Using %d as default.\n", str, default_value);
		return default_value;
	}

	if ((errno == ERANGE && (lval == LONG_MAX || lval == LONG_MIN)) ||
	    (lval > INT_MAX || lval < INT_MIN)) {
		fprintf(stderr, "%s out of range! Using %d as default.\n", str, default_value);
		return default_value;
	}

	return lval;
}

/* Like parse_integer but recognize the percentages (i.e. strings ending with `%') */
int
parse_int_with_percentage(const char *str, int default_value, int max)
{
	int len = strlen(str);

	if (len > 0 && str[len-1] == '%') {
		int val;
		char *cpy;

		cpy = strdup(str);
		check_allocation(cpy);
		cpy[len-1] = '\0';
		val = parse_integer(cpy, default_value);
		free(cpy);
		return val * max / 100;
	}

	return parse_integer(str, default_value);
}

/* 
 * Like parse_int_with_percentage but understands some special values:
 * - middle that is (max-self)/2
 * - start  that is 0
 * - end    that is (max-self)
 */
int
parse_int_with_pos(const char *str, int default_value, int max, int self)
{
	if (!strcmp(str, "start"))
		return 0;
	if (!strcmp(str, "middle"))
		return (max - self)/2;
	if (!strcmp(str, "end"))
		return max-self;
	return parse_int_with_percentage(str, default_value, max);
}

/* Parse a string like a CSS value. */
/* TODO: harden a bit this function */
char **
parse_csslike(const char *str)
{
	int	i, j;
	char	*s, *token, **ret;
	short	any_null;

	s = strdup(str);
	if (s == NULL)
		return NULL;

	ret = malloc(4 * sizeof(char*));
	if (ret == NULL) {
		free(s);
		return NULL;
	}

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

	/* before we didn't check for the return type of strdup, here we will */

	any_null = 0;
	for (i = 0; i < 4; ++i)
		any_null = ret[i] == NULL || any_null;

	if (any_null)
		for (i = 0; i < 4; ++i)
			if (ret[i] != NULL)
				free(ret[i]);

	if (i == 0 || any_null) {
		free(s);
		free(ret);
		return NULL;
	}

	return ret;
}

/* 
 * Given an event, try to understand what the users wants. If the
 * return value is ADD_CHAR then `input' is a pointer to a string that
 * will need to be free'ed later.
 */
enum
action parse_event(Display *d, XKeyPressedEvent *ev, XIC xic, char **input)
{
	char	str[SYM_BUF_SIZE] = {0};
	Status	s;

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
		fprintf(stderr, "Buffer overflow when trying to create keyboard symbol map.\n");
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

void
confirm(enum state *status, struct rendering *r, struct completions *cs, char **text, int *textlen)
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

/* event loop */
enum state
loop(struct rendering *r, char **text, int *textlen, struct completions *cs, char **lines, char **vlines)
{
	enum state status = LOOPING;

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

		case KeyPress: {
			XKeyPressedEvent *ev = (XKeyPressedEvent*)&e;
			char *input;

			switch (parse_event(r->d, ev, r->xic, &input)) {
			case EXIT:
				status = ERR;
				break;

			case CONFIRM: {
				status = OK;
				confirm(&status, r, cs, text, textlen);
				break;
			}

			case CONFIRM_CONTINUE: {
				status = OK_LOOP;
				confirm(&status, r, cs, text, textlen);
				break;
			}

			case PREV_COMPL: {
				complete(cs, r->first_selected, 1, text, textlen, &status);
				r->offset = cs->selected;
				break;
			}

			case NEXT_COMPL: {
				complete(cs, r->first_selected, 0, text, textlen, &status);
				r->offset = cs->selected;
				break;
			}

			case DEL_CHAR:
				popc(*text);
				update_completions(cs, *text, lines, vlines, r->first_selected);
				r->offset = 0;
				break;

			case DEL_WORD: {
				popw(*text);
				update_completions(cs, *text, lines, vlines, r->first_selected);
				break;
			}

			case DEL_LINE: {
				int i;
				for (i = 0; i < *textlen; ++i)
					*(*text + i) = 0;
				update_completions(cs, *text, lines, vlines, r->first_selected);
				r->offset = 0;
				break;
			}

			case ADD_CHAR: {
				int str_len, i;

				str_len = strlen(input);

				/*
				 * sometimes a strange key is pressed
				 * i.e. ctrl alone), so input will be
				 * empty. Don't need to update
				 * completion in that case
				 */
				if (str_len == 0)
					break;

				for (i = 0; i < str_len; ++i) {
					*textlen = pushc(text, *textlen, input[i]);
					if (*textlen == -1) {
						fprintf(stderr, "Memory allocation error\n");
						status = ERR;
						break;
					}
				}

				if (status != ERR) {
					update_completions(cs, *text, lines, vlines, r->first_selected);
					free(input);
				}

				r->offset = 0;
				break;
			}

			case TOGGLE_FIRST_SELECTED:
				r->first_selected = !r->first_selected;
				if (r->first_selected && cs->selected < 0)
					cs->selected = 0;
				if (!r->first_selected && cs->selected == 0)
					cs->selected = -1;
				break;
			}
		}

		case ButtonPress: {
			XButtonPressedEvent *ev = (XButtonPressedEvent*)&e;
			/* if (ev->button == Button1) { /\* click *\/ */
			/*   int x = ev->x - r.border_w; */
			/*   int y = ev->y - r.border_n; */
			/*   fprintf(stderr, "Click @ (%d, %d)\n", x, y); */
			/* } */

			if (ev->button == Button4) /* scroll up */
				r->offset = MAX((ssize_t)r->offset - 1, 0);

			if (ev->button == Button5) /* scroll down */
				r->offset = MIN(r->offset + 1, cs->length - 1);

			break;
		}
		}

		draw(r, *text, cs);
	}

	return status;
}

int
load_font(struct rendering *r, const char *fontname)
{
#ifdef USE_XFT
	r->font = XftFontOpenName(r->d, DefaultScreen(r->d), fontname);
	return 0;
#else
	char **missing_charset_list;
	int missing_charset_count;

	r->font = XCreateFontSet(r->d, fontname, &missing_charset_list, &missing_charset_count, NULL);
	if (r->font != NULL)
		return 0;

	fprintf(stderr, "Unable to load the font(s) %s\n", fontname);

	if (!strcmp(fontname, default_fontname))
		return -1;

	return load_font(r, default_fontname);
#endif
}

void
xim_init(struct rendering *r, XrmDatabase *xdb)
{
	XIM		xim;
	XIMStyle	best_match_style;
	XIMStyles	*xis;
	int		i;

	/* Open the X input method */
	xim = XOpenIM(r->d, *xdb, resname, resclass);
	check_allocation(xim);

	if (XGetIMValues(xim, XNQueryInputStyle, &xis, NULL) || !xis) {
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
		fprintf(stderr, "No matching input style could be determined\n");

	r->xic = XCreateIC(xim, XNInputStyle, best_match_style, XNClientWindow, r->w, XNFocusWindow, r->w, NULL);
	check_allocation(r->xic);
}

void
create_window(struct rendering *r, Window parent_window, Colormap cmap, XVisualInfo vinfo, int x, int y, int ox, int oy)
{
	XSetWindowAttributes	attr;

	/* Create the window */
	attr.colormap = cmap;
	attr.override_redirect = 1;
	attr.border_pixel = 0;
	attr.background_pixel = 0x80808080;
	attr.event_mask = ExposureMask | KeyPressMask | VisibilityChangeMask;

	r->w = XCreateWindow(r->d,
		parent_window,
		x + ox, y + oy,
		r->width, r->height,
		0,
		vinfo.depth,
		InputOutput,
		vinfo.visual,
		CWBorderPixel | CWBackPixel | CWColormap | CWEventMask | CWOverrideRedirect,
		&attr);
}

void
ps1extents(struct rendering *r)
{
	char *dup;
	dup = strdupn(r->ps1);
	text_extents(dup == NULL ? r->ps1 : dup, r->ps1len, r, &r->ps1w, &r->ps1h);
	free(dup);
}

void
usage(char *prgname)
{
	fprintf(stderr,
		"%s [-Aahmv] [-B colors] [-b size] [-C color] [-c color]\n"
		"       [-d separator] [-e window] [-f font] [-G color] [-g size]\n"
		"       [-H height] [-I color] [-i size] [-J color] [-j size] [-l layout]\n"
		"       [-P padding] [-p prompt] [-S color] [-s color] [-T color]\n"
		"       [-t color] [-W width] [-x coord] [-y coord]\n", prgname);
}

int
main(int argc, char **argv)
{
	struct completions	*cs;
	struct rendering	r;
	XVisualInfo		vinfo;
	Colormap		cmap;
	size_t			nlines, i;
	Window			parent_window;
	XrmDatabase		xdb;
	unsigned long		fgs[3], bgs[3]; /* prompt, compl, compl_highlighted */
	unsigned long		borders_bg[4], p_borders_bg[4], c_borders_bg[4], ch_borders_bg[4]; /* N E S W */
	enum state		status;
	int			ch;
	int			offset_x, offset_y, x, y;
	int			textlen, d_width, d_height;
	short			embed;
	char			*sep, *parent_window_id;
	char			**lines, *buf, **vlines;
	char			*fontname, *text, *xrm;

#ifdef __OpenBSD__
	/* stdio & rpath: to read/write stdio/stdout/stderr */
	/* unix:          to connect to XOrg */
	pledge("stdio rpath unix", "");
#endif

	sep = NULL;
	parent_window_id = NULL;

	r.first_selected = 0;
	r.free_text = 1;
	r.multiple_select = 0;
	r.offset = 0;

	while ((ch = getopt(argc, argv, ARGS)) != -1) {
		switch (ch) {
		case 'h':	/* help */
			usage(*argv);
			return 0;
		case 'v':	/* version */
			fprintf(stderr, "%s version: %s\n", *argv, VERSION);
			return 0;
		case 'e':	/* embed */
			parent_window_id = strdup(optarg);
			check_allocation(parent_window_id);
			break;
		case 'd':
			sep = strdup(optarg);
			check_allocation(sep);
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

	/* Read the completions */
	lines = NULL;
	buf = NULL;
	nlines = readlines(&lines, &buf);

	vlines = NULL;
	if (sep != NULL) {
		int l;
		l = strlen(sep);
		vlines = calloc(nlines, sizeof(char*));
		check_allocation(vlines);

		for (i = 0; i < nlines; i++) {
			char *t;
			t = strstr(lines[i], sep);
			if (t == NULL)
				vlines[i] = lines[i];
			else
				vlines[i] = t + l;
		}
	}

	setlocale(LC_ALL, getenv("LANG"));

	status = LOOPING;

	/* where the monitor start (used only with xinerama) */
	offset_x = offset_y = 0;

	/* default width and height */
	r.width = 400;
	r.height = 20;

	/* default position on the screen */
	x = y = 0;

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

	/* the prompt. We duplicate the string so later is easy to
	 * free (in the case it's been overwritten by the user) */
	r.ps1 = strdup("$ ");
	check_allocation(r.ps1);

	/* same for the font name */
	fontname = strdup(default_fontname);
	check_allocation(fontname);

	textlen = 10;
	text = malloc(textlen * sizeof(char));
	check_allocation(text);

	/* struct completions *cs = filter(text, lines); */
	cs = compls_new(nlines);
	check_allocation(cs);

	/* start talking to xorg */
	r.d = XOpenDisplay(NULL);
	if (r.d == NULL) {
		fprintf(stderr, "Could not open display!\n");
		return EX_UNAVAILABLE;
	}

	embed = 1;
	if (! (parent_window_id && (parent_window = strtol(parent_window_id, NULL, 0)))) {
		parent_window = DefaultRootWindow(r.d);
		embed = 0;
	}

	/* get display size */
	get_wh(r.d, &parent_window, &d_width, &d_height);

#ifdef USE_XINERAMA
	if (!embed && XineramaIsActive(r.d)) { /* find the mice */
		XineramaScreenInfo *info;
		Window		rr;
		Window		root;
		int		number_of_screens, monitors, i;
		int		root_x, root_y, win_x, win_y;
		unsigned int	mask;
		short		res;

		number_of_screens = XScreenCount(r.d);
		for (i = 0; i < number_of_screens; ++i) {
			root = XRootWindow(r.d, i);
			res = XQueryPointer(r.d, root, &rr, &rr, &root_x, &root_y, &win_x, &win_y, &mask);
			if (res)
				break;
		}

		if (!res) {
			fprintf(stderr, "No mouse found.\n");
			root_x = 0;
			root_y = 0;
		}

		/* Now find in which monitor the mice is */
		info = XineramaQueryScreens(r.d, &monitors);
		if (info) {
			for (i = 0; i < monitors; ++i) {
				if (info[i].x_org <= root_x && root_x <= (info[i].x_org + info[i].width)
				    && info[i].y_org <= root_y && root_y <= (info[i].y_org + info[i].height)) {
					offset_x = info[i].x_org;
					offset_y = info[i].y_org;
					d_width = info[i].width;
					d_height = info[i].height;
					break;
				}
			}
		}
		XFree(info);
	}
#endif

	XMatchVisualInfo(r.d, DefaultScreen(r.d), 32, TrueColor, &vinfo);
	cmap = XCreateColormap(r.d, XDefaultRootWindow(r.d), vinfo.visual, AllocNone);

	fgs[0] = fgs[1] = parse_color("#fff", NULL);
	fgs[2] = parse_color("#000", NULL);

	bgs[0] = bgs[1] = parse_color("#000", NULL);
	bgs[2] = parse_color("#fff", NULL);

	borders_bg[0] = borders_bg[1] = borders_bg[2] = borders_bg[3] = parse_color("#000", NULL);

	p_borders_bg[0]  = p_borders_bg[1]  = p_borders_bg[2]  = p_borders_bg[3]  = parse_color("#000", NULL);
	c_borders_bg[0]  = c_borders_bg[1]  = c_borders_bg[2]  = c_borders_bg[3]  = parse_color("#000", NULL);
	ch_borders_bg[0] = ch_borders_bg[1] = ch_borders_bg[2] = ch_borders_bg[3] = parse_color("#000", NULL);

	r.horizontal_layout = 1;

	/* Read the resources */
	XrmInitialize();
	xrm = XResourceManagerString(r.d);
	xdb = NULL;
	if (xrm != NULL) {
		XrmValue value;
		char *datatype[20];

		xdb = XrmGetStringDatabase(xrm);

		if (XrmGetResource(xdb, "MyMenu.font", "*", datatype, &value) == 1) {
			free(fontname);
			fontname = strdup(value.addr);
			check_allocation(fontname);
		} else {
			fprintf(stderr, "no font defined, using %s\n", fontname);
		}

		if (XrmGetResource(xdb, "MyMenu.layout", "*", datatype, &value) == 1)
			r.horizontal_layout = !strcmp(value.addr, "horizontal");
		else
			fprintf(stderr, "no layout defined, using horizontal\n");

		if (XrmGetResource(xdb, "MyMenu.prompt", "*", datatype, &value) == 1) {
			free(r.ps1);
			r.ps1 = normalize_str(value.addr);
		} else {
			fprintf(stderr, "no prompt defined, using \"%s\" as default\n", r.ps1);
		}

		if (XrmGetResource(xdb, "MyMenu.prompt.border.size", "*", datatype, &value) == 1) {
			char **sizes;
			sizes = parse_csslike(value.addr);
			if (sizes != NULL)
				for (i = 0; i < 4; ++i)
					r.p_borders[i] = parse_integer(sizes[i], 0);
			else
				fprintf(stderr, "error while parsing MyMenu.prompt.border.size");
		}

		if (XrmGetResource(xdb, "MyMenu.prompt.border.color", "*", datatype, &value) == 1) {
			char **colors;
			colors = parse_csslike(value.addr);
			if (colors != NULL)
				for (i = 0; i < 4; ++i)
					p_borders_bg[i] = parse_color(colors[i], "#000");
			else
				fprintf(stderr, "error while parsing MyMenu.prompt.border.color");
		}

		if (XrmGetResource(xdb, "MyMenu.prompt.padding", "*", datatype, &value) == 1) {
			char **colors;
			colors = parse_csslike(value.addr);
			if (colors != NULL)
				for (i = 0; i < 4; ++i)
					r.p_padding[i] = parse_integer(colors[i], 0);
			else
				fprintf(stderr, "error while parsing MyMenu.prompt.padding");
		}

		if (XrmGetResource(xdb, "MyMenu.width", "*", datatype, &value) == 1)
			r.width = parse_int_with_percentage(value.addr, r.width, d_width);
		else
			fprintf(stderr, "no width defined, using %d\n", r.width);

		if (XrmGetResource(xdb, "MyMenu.height", "*", datatype, &value) == 1)
			r.height = parse_int_with_percentage(value.addr, r.height, d_height);
		else
			fprintf(stderr, "no height defined, using %d\n", r.height);

		if (XrmGetResource(xdb, "MyMenu.x", "*", datatype, &value) == 1)
			x = parse_int_with_pos(value.addr, x, d_width, r.width);

		if (XrmGetResource(xdb, "MyMenu.y", "*", datatype, &value) == 1)
			y = parse_int_with_pos(value.addr, y, d_height, r.height);

		if (XrmGetResource(xdb, "MyMenu.border.size", "*", datatype, &value) == 1) {
			char **borders;
			borders = parse_csslike(value.addr);
			if (borders != NULL)
				for (i = 0; i < 4; ++i)
					r.borders[i] = parse_int_with_percentage(borders[i], 0, (i % 2) == 0 ? d_height : d_width);
			else
				fprintf(stderr, "error while parsing MyMenu.border.size\n");
		}

		/* Prompt */
		if (XrmGetResource(xdb, "MyMenu.prompt.foreground", "*", datatype, &value) == 1)
			fgs[0] = parse_color(value.addr, "#fff");

		if (XrmGetResource(xdb, "MyMenu.prompt.background", "*", datatype, &value) == 1)
			bgs[0] = parse_color(value.addr, "#000");

		/* Completions */
		if (XrmGetResource(xdb, "MyMenu.completion.foreground", "*", datatype, &value) == 1)
			fgs[1] = parse_color(value.addr, "#fff");

		if (XrmGetResource(xdb, "MyMenu.completion.background", "*", datatype, &value) == 1)
			bgs[1] = parse_color(value.addr, "#000");

		if (XrmGetResource(xdb, "MyMenu.completion.padding", "*", datatype, &value) == 1) {
			char **paddings;
			paddings = parse_csslike(value.addr);
			if (paddings != NULL)
				for (i = 0; i < 4; ++i)
					r.c_padding[i] = parse_integer(paddings[i], 0);
			else
				fprintf(stderr, "Error while parsing MyMenu.completion.padding");
		}

		if (XrmGetResource(xdb, "MyMenu.completion.border.size", "*", datatype, &value) == 1) {
			char **sizes;
			sizes = parse_csslike(value.addr);
			if (sizes != NULL)
				for (i = 0; i < 4; ++i)
					r.c_borders[i] = parse_integer(sizes[i], 0);
			else
				fprintf(stderr, "Error while parsing MyMenu.completion.border.size");
		}

		if (XrmGetResource(xdb, "MyMenu.completion.border.color", "*", datatype, &value) == 1) {
			char **sizes;
			sizes = parse_csslike(value.addr);
			if (sizes != NULL)
				for (i = 0; i < 4; ++i)
					c_borders_bg[i] = parse_color(sizes[i], "#000");
			else
				fprintf(stderr, "Error while parsing MyMenu.completion.border.color");
		}

		/* Completion Highlighted */
		if (XrmGetResource(xdb, "MyMenu.completion_highlighted.foreground", "*", datatype, &value) == 1)
			fgs[2] = parse_color(value.addr, "#000");

		if (XrmGetResource(xdb, "MyMenu.completion_highlighted.background", "*", datatype, &value) == 1)
			bgs[2] = parse_color(value.addr, "#fff");

		if (XrmGetResource(xdb, "MyMenu.completion_highlighted.padding", "*", datatype, &value) == 1) {
			char **paddings;
			paddings = parse_csslike(value.addr);
			if (paddings != NULL)
				for (i = 0; i < 4; ++i)
					r.ch_padding[i] = parse_integer(paddings[i], 0);
			else
				fprintf(stderr, "Error while parsing MyMenu.completion_highlighted.padding");
		}

		if (XrmGetResource(xdb, "MyMenu.completion_highlighted.border.size", "*", datatype, &value) == 1) {
			char **sizes;
			sizes = parse_csslike(value.addr);
			if (sizes != NULL)
				for (i = 0; i < 4; ++i)
					r.ch_borders[i] = parse_integer(sizes[i], 0);
			else
				fprintf(stderr, "Error while parsing MyMenu.completion_highlighted.border.size");
		}

		if (XrmGetResource(xdb, "MyMenu.completion_highlighted.border.color", "*", datatype, &value) == 1) {
			char **colors;
			colors = parse_csslike(value.addr);
			if (colors != NULL)
				for (i = 0; i < 4; ++i)
					ch_borders_bg[i] = parse_color(colors[i], "#000");
			else
				fprintf(stderr, "Error while parsing MyMenu.completion_highlighted.border.color");
		}

		/* Border */
		if (XrmGetResource(xdb, "MyMenu.border.color", "*", datatype, &value) == 1) {
			char **colors;
			colors = parse_csslike(value.addr);
			if (colors != NULL)
				for (i = 0; i < 4; ++i)
					borders_bg[i] = parse_color(colors[i], "#000");
			else
				fprintf(stderr, "error while parsing MyMenu.border.color\n");
		}
	}

	/* Second round of args parsing */
	optind = 0;		/* reset the option index */
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
			/* multiple selection this case was already catched. */
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
			x = parse_int_with_pos(optarg, x, d_width, r.width);
			break;
		case 'y':
			y = parse_int_with_pos(optarg, y, d_height, r.height);
			break;
		case 'P': {
			char **paddings;
			if ((paddings = parse_csslike(optarg)) != NULL)
				for (i = 0; i < 4; ++i)
					r.p_padding[i] = parse_integer(paddings[i], 0);
			break;
		}
		case 'G': {
			char **colors;
			if ((colors = parse_csslike(optarg)) != NULL)
				for (i = 0; i < 4; ++i)
					p_borders_bg[i] = parse_color(colors[i], "#000");
			break;
		}
		case 'g': {
			char **sizes;
			if ((sizes = parse_csslike(optarg)) != NULL)
				for (i = 0; i < 4; ++i)
					r.p_borders[i] = parse_integer(sizes[i], 0);
			break;
		}
		case 'I': {
			char **colors;
			if ((colors = parse_csslike(optarg)) != NULL)
				for (i = 0; i < 4; ++i)
					c_borders_bg[i] = parse_color(colors[i], "#000");
			break;
		}
		case 'i': {
			char **sizes;
			if ((sizes = parse_csslike(optarg)) != NULL)
				for (i = 0; i < 4; ++i)
					r.c_borders[i] = parse_integer(sizes[i], 0);
			break;
		}
		case 'J': {
			char **colors;
			if ((colors = parse_csslike(optarg)) != NULL)
				for (i = 0; i < 4; ++i)
					ch_borders_bg[i] = parse_color(colors[i], "#000");
			break;
		}
		case 'j': {
			char **sizes;
			if ((sizes = parse_csslike(optarg)) != NULL)
				for (i = 0; i < 4; ++i)
					r.ch_borders[i] = parse_integer(sizes[i], 0);
			break;
		}
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
		case 'b': {
			char **borders;
			if ((borders = parse_csslike(optarg)) != NULL) {
				for (i = 0; i < 4; ++i)
					r.borders[i] = parse_integer(borders[i], 0);
			} else
				fprintf(stderr, "Error parsing b option\n");
			break;
		}
		case 'B': {
			char **colors;
			if ((colors = parse_csslike(optarg)) != NULL) {
				for (i = 0; i < 4; ++i)
					borders_bg[i] = parse_color(colors[i], "#000");
			} else
				fprintf(stderr, "error while parsing B option\n");
			break;
		}
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
			fgs[2] = parse_color(optarg, NULL);
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

	/* update the prompt lenght, only now we surely know the length of it */
	r.ps1len = strlen(r.ps1);

	/* Create the window */
	create_window(&r, parent_window, cmap, vinfo, x, y, offset_x, offset_y);
	set_win_atoms_hints(r.d, r.w, r.width, r.height);
	XSelectInput(r.d, r.w, StructureNotifyMask | KeyPressMask | KeymapStateMask | ButtonPressMask);
	XMapRaised(r.d, r.w);

	/* If embed, listen for other events as well */
	if (embed) {
		Window		*children, parent, root;
		unsigned int	children_no;

		XSelectInput(r.d, parent_window, FocusChangeMask);
		if (XQueryTree(r.d, parent_window, &root, &parent, &children, &children_no) && children) {
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
		XGCValues	values;

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

#ifdef USE_XFT
	r.xftdraw = XftDrawCreate(r.d, r.w, vinfo.visual, cmap);

	{
		rgba_t c;
		XRenderColor xrcolor;

		/* Prompt */
		c = *(rgba_t*)&fgs[0];
		xrcolor.red          = EXPANDBITS(c.rgba.r);
		xrcolor.green        = EXPANDBITS(c.rgba.g);
		xrcolor.blue         = EXPANDBITS(c.rgba.b);
		xrcolor.alpha        = EXPANDBITS(c.rgba.a);
		XftColorAllocValue(r.d, DefaultVisual(r.d, 0), DefaultColormap(r.d, 0), &xrcolor, &r.xft_colors[0]);

		/* Completion */
		c = *(rgba_t*)&fgs[1];
		xrcolor.red          = EXPANDBITS(c.rgba.r);
		xrcolor.green        = EXPANDBITS(c.rgba.g);
		xrcolor.blue         = EXPANDBITS(c.rgba.b);
		xrcolor.alpha        = EXPANDBITS(c.rgba.a);
		XftColorAllocValue(r.d, DefaultVisual(r.d, 0), DefaultColormap(r.d, 0), &xrcolor, &r.xft_colors[1]);

		/* Completion highlighted */
		c = *(rgba_t*)&fgs[2];
		xrcolor.red          = EXPANDBITS(c.rgba.r);
		xrcolor.green        = EXPANDBITS(c.rgba.g);
		xrcolor.blue         = EXPANDBITS(c.rgba.b);
		xrcolor.alpha        = EXPANDBITS(c.rgba.a);
		XftColorAllocValue(r.d, DefaultVisual(r.d, 0), DefaultColormap(r.d, 0), &xrcolor, &r.xft_colors[2]);
	}
#endif

	/* compute prompt dimensions */
	ps1extents(&r);

	xim_init(&r, &xdb);

#ifdef __OpenBSD__
	/* Now we need only the ability to write */
	pledge("stdio", "");
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

#ifdef USE_XFT
	XftColorFree(r.d, DefaultVisual(r.d, 0), DefaultColormap(r.d, 0), &r.xft_colors[0]);
	XftColorFree(r.d, DefaultVisual(r.d, 0), DefaultColormap(r.d, 0), &r.xft_colors[1]);
	XftColorFree(r.d, DefaultVisual(r.d, 0), DefaultColormap(r.d, 0), &r.xft_colors[2]);
#endif

	free(r.ps1);
	free(fontname);
	free(text);

	free(buf);
	free(lines);
	free(vlines);
	compls_delete(cs);

	XDestroyWindow(r.d, r.w);
	XCloseDisplay(r.d);

	return status != OK;
}
