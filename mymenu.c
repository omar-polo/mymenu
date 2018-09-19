#include <stdio.h>
#include <stdlib.h>
#include <string.h>    // strdup, strnlen, ...
#include <ctype.h>     // isalnum
#include <locale.h>    // setlocale
#include <unistd.h>
#include <sysexits.h>
#include <stdbool.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h> // XLookupString
#include <X11/Xresource.h>
#include <X11/Xcms.h>  // colors
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

// Comfy
#define nil NULL

#define resname "MyMenu"
#define resclass "mymenu"

#define SYM_BUF_SIZE 4

#ifdef USE_XFT
# define default_fontname "monospace"
#else
# define default_fontname "fixed"
#endif

#define ARGS "Aahmve:p:P:l:f:W:H:x:y:b:B:t:T:c:C:s:S:d:"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define EXPANDBITS(x)   ((0xffff * x) / 0xff)

// If we don't have it or we don't want an "ignore case" completion
// style, fall back to `strstr(3)`
#ifndef USE_STRCASESTR
# define strcasestr strstr
#endif

// The number of char to read
#define STDIN_CHUNKS 64

// the number of lines to allocate in advance
#define LINES_CHUNK 32

// Abort if a is nil
#define check_allocation(a) {                         \
    if (a == nil) {                                   \
      fprintf(stderr, "Could not allocate memory\n"); \
      abort();                                        \
    }                                                 \
  }

#define inner_height(r) (r->height - r->border_n - r->border_s)
#define inner_width(r)  (r->width  - r->border_e - r->border_w)

// The possible state of the event loop.
enum state {LOOPING, OK_LOOP, OK, ERR};

// for the drawing-related function. The text to be rendered could be
// the prompt, a completion or a highlighted completion
enum text_type {PROMPT, COMPL, COMPL_HIGH};

// These are the possible action to be performed after user input.
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

// A big set of values that needs to be carried around (for drawing
// functions). A struct to rule them all
struct rendering {
  Display *d; // connection to xorg
  Window w;
  int width;
  int height;
  int padding;
  int x_zero; // the "zero" on the x axis (may not be 0 'cause the border)
  int y_zero; // the same a x_zero, only for the y axis

  size_t offset; // a scrolling offset

  bool free_text;
  bool first_selected;
  bool multiple_select;

  // The four border
  int border_n;
  int border_e;
  int border_s;
  int border_w;

  bool horizontal_layout;

  // the prompt
  char *ps1;
  int ps1len;

  XIC xic;

  // colors
  GC prompt;
  GC prompt_bg;
  GC completion;
  GC completion_bg;
  GC completion_highlighted;
  GC completion_highlighted_bg;
  GC border_n_bg;
  GC border_e_bg;
  GC border_s_bg;
  GC border_w_bg;
#ifdef USE_XFT
  XftFont *font;
  XftDraw *xftdraw;
  XftColor xft_prompt;
  XftColor xft_completion;
  XftColor xft_completion_highlighted;
#else
  XFontSet *font;
#endif
};

struct completion {
  char *completion;
  char *rcompletion;
};

// Wrap the linked list of completions
struct completions {
  struct completion *completions;
  ssize_t selected;
  size_t lenght;
};

// return a newly allocated (and empty) completion list
struct completions *compls_new(size_t lenght) {
  struct completions *cs = malloc(sizeof(struct completions));

  if (cs == nil)
    return cs;

  cs->completions = calloc(lenght, sizeof(struct completion));
  if (cs->completions == nil) {
    free(cs);
    return nil;
  }

  cs->selected = -1;
  cs->lenght = lenght;
  return cs;
}

/* idea stolen from lemonbar. ty lemonboy */
typedef union {
  struct {
    uint8_t b;
    uint8_t g;
    uint8_t r;
    uint8_t a;
  };
  uint32_t v;
} rgba_t;

// Delete the wrapper and the whole list
void compls_delete(struct completions *cs) {
  if (cs == nil)
    return;

  free(cs->completions);
  free(cs);
}

// create a completion list from a text and the list of possible
// completions (null terminated). Expects a non-null `cs'. lines and
// vlines should have the same lenght OR vlines is null
void filter(struct completions *cs, char *text, char **lines, char **vlines) {
  size_t index = 0;
  size_t matching = 0;

  if (vlines == nil)
    vlines = lines;

  while (true) {
    char *l = vlines[index] != nil ? vlines[index] : lines[index];
    if (l == nil)
      break;

    if (strcasestr(l, text) != nil) {
      struct completion *c = &cs->completions[matching];
      c->completion  = l;
      c->rcompletion = lines[index];
      matching++;
    }

    index++;
  }
  cs->lenght = matching;
  cs->selected = -1;
}

// update the given completion, that is: clean the old cs & generate a new one.
void update_completions(struct completions *cs, char *text, char **lines, char **vlines, bool first_selected) {
  filter(cs, text, lines, vlines);
  if (first_selected && cs->lenght > 0)
    cs->selected = 0;
}

// select the next, or the previous, selection and update some
// state. `text' will be updated with the text of the completion and
// `textlen' with the new lenght of `text'. If the memory cannot be
// allocated, `status' will be set to `ERR'.
void complete(struct completions *cs, bool first_selected, bool p, char **text, int *textlen, enum state *status) {
  if (cs == nil || cs->lenght == 0)
    return;

  // if the first is always selected, and the first entry is different
  // from the text, expand the text and return
  if (first_selected
      && cs->selected == 0
      && strcmp(cs->completions->completion, *text) != 0
      && !p) {
    free(*text);
    *text = strdup(cs->completions->completion);
    if (text == nil) {
      *status = ERR;
      return;
    }
    *textlen = strlen(*text);
    return;
  }

  int index = cs->selected;

  if (index == -1 && p)
    index = 0;
  index = cs->selected = (cs->lenght + (p ? index - 1 : index + 1)) % cs->lenght;

  struct completion *n = &cs->completions[cs->selected];

  free(*text);
  *text = strdup(n->completion);
  if (text == nil) {
    fprintf(stderr, "Memory allocation error!\n");
    *status = ERR;
    return;
  }
  *textlen = strlen(*text);
}

// push the character c at the end of the string pointed by p
int pushc(char **p, int maxlen, char c) {
  int len = strnlen(*p, maxlen);

  if (!(len < maxlen -2)) {
    maxlen += maxlen >> 1;
    char *newptr = realloc(*p, maxlen);
    if (newptr == nil) { // bad!
      return -1;
    }
    *p = newptr;
  }

  (*p)[len] = c;
  (*p)[len+1] = '\0';
  return maxlen;
}

// remove the last rune from the *utf8* string! This is different from
// just setting the last byte to 0 (in some cases ofc). Return a
// pointer (e) to the last non zero char. If e < p then p is empty!
char* popc(char *p) {
  int len = strlen(p);
  if (len == 0)
    return p;

  char *e = p + len - 1;

  do {
    char c = *e;
    *e = 0;
    e--;

    // if c is a starting byte (11......) or is under U+007F (ascii,
    // basically) we're done
    if (((c & 0x80) && (c & 0x40)) || !(c & 0x80))
      break;
  } while (e >= p);

  return e;
}

// remove the last word plus trailing whitespaces from the give string
void popw(char *w) {
  int len = strlen(w);
  if (len == 0)
    return;

  bool in_word = true;
  while (true) {
    char *e = popc(w);

    if (e < w)
      return;

    if (in_word && isspace(*e))
      in_word = false;

    if (!in_word && !isspace(*e))
      return;
  }
}

// If the string is surrounded by quotes (`"`) remove them and replace
// every `\"` in the string with `"`
char *normalize_str(const char *str) {
  int len = strlen(str);
  if (len == 0)
    return nil;

  char *s = calloc(len, sizeof(char));
  check_allocation(s);
  int p = 0;
  while (*str) {
    char c = *str;
    if (*str == '\\') {
      if (*(str + 1)) {
        s[p] = *(str + 1);
        p++;
        str += 2; // skip this and the next char
        continue;
      } else {
        break;
      }
    }
    if (c == '"') {
      str++; // skip only this char
      continue;
    }
    s[p] = c;
    p++;
    str++;
  }
  return s;
}

size_t read_stdin(char **buf) {
  size_t offset = 0;
  size_t len = STDIN_CHUNKS;
  *buf = malloc(len * sizeof(char));
  if (*buf == nil)
    goto err;

  while (true) {
    ssize_t r = read(0, *buf + offset, STDIN_CHUNKS);
    if (r < 1)
      return len;

    offset += r;

    len += STDIN_CHUNKS;
    *buf = realloc(*buf, len);
    if (*buf == nil)
      goto err;

    for (size_t i = offset; i < len; ++i)
      (*buf)[i] = '\0';
  }

 err:
  fprintf(stderr, "Error in allocating memory for stdin.\n");
  exit(EX_UNAVAILABLE);
}

//
size_t readlines(char ***lns, char **buf) {
  *buf = nil;
  size_t len = read_stdin(buf);

  size_t ll = LINES_CHUNK;
  *lns = malloc(ll * sizeof(char*));
  size_t lines = 0;
  bool in_line = false;
  for (size_t i = 0; i < len; i++) {
    char c = (*buf)[i];

    if (c == '\0')
      break;

    if (c == '\n')
      (*buf)[i] = '\0';

    if (in_line && c == '\n')
      in_line = false;

    if (!in_line && c != '\n') {
      in_line = true;
      (*lns)[lines] = (*buf) + i;
      lines++;

      if (lines == ll) { // resize
        ll += LINES_CHUNK;
        *lns = realloc(*lns, ll * sizeof(char*));
        if (*lns == nil) {
          fprintf(stderr, "Error in memory allocation.\n");
          exit(EX_UNAVAILABLE);
        }
      }
    }
  }

  return lines;
}

// Compute the dimension of the string str once rendered, return the
// width and save the width and the height in ret_width and ret_height
int text_extents(char *str, int len, struct rendering *r, int *ret_width, int *ret_height) {
  int height;
  int width;
#ifdef USE_XFT
  XGlyphInfo gi;
  XftTextExtentsUtf8(r->d, r->font, str, len, &gi);
  height = r->font->ascent - r->font->descent;
  width = gi.width - gi.x;
#else
  XRectangle rect;
  XmbTextExtents(*r->font, str, len, nil, &rect);
  height = rect.height;
  width = rect.width;
#endif
  if (ret_width != nil)  *ret_width = width;
  if (ret_height != nil) *ret_height = height;
  return width;
}

// Draw the string str
void draw_string(char *str, int len, int x, int y, struct rendering *r, enum text_type tt) {
#ifdef USE_XFT
  XftColor xftcolor;
  if (tt == PROMPT)     xftcolor = r->xft_prompt;
  if (tt == COMPL)      xftcolor = r->xft_completion;
  if (tt == COMPL_HIGH) xftcolor = r->xft_completion_highlighted;

  XftDrawStringUtf8(r->xftdraw, &xftcolor, r->font, x, y, str, len);
#else
  GC gc;
  if (tt == PROMPT)     gc = r->prompt;
  if (tt == COMPL)      gc = r->completion;
  if (tt == COMPL_HIGH) gc = r->completion_highlighted;
  Xutf8DrawString(r->d, r->w, *r->font, gc, x, y, str, len);
#endif
}

// Duplicate the string str and substitute every space with a 'n'
char *strdupn(char *str) {
  int len = strlen(str);

  if (str == nil || len == 0)
    return nil;

  char *dup = strdup(str);
  if (dup == nil)
    return nil;

  for (int i = 0; i < len; ++i)
    if (dup[i] == ' ')
      dup[i] = 'n';

  return dup;
}

// |------------------|----------------------------------------------|
// | 20 char text     | completion | completion | completion | compl |
// |------------------|----------------------------------------------|
void draw_horizontally(struct rendering *r, char *text, struct completions *cs) {
  int prompt_width = 20; // char

  char *ps1dup = strdupn(r->ps1);
  int width, height;
  int ps1xlen = text_extents(ps1dup != nil ? ps1dup : r->ps1, r->ps1len, r, &width, &height);
  free(ps1dup);
  int start_at = ps1xlen;

  start_at = r->x_zero + text_extents("n", 1, r, nil, nil);
  start_at = start_at * prompt_width + r->padding;

  int texty = (inner_height(r) + height + r->y_zero) / 2;

  XFillRectangle(r->d, r->w, r->prompt_bg, r->x_zero, r->y_zero, start_at, inner_height(r));

  int text_len = strlen(text);
  if (text_len > prompt_width)
    text = text + (text_len - prompt_width);
  draw_string(r->ps1, r->ps1len, r->x_zero + r->padding, texty, r, PROMPT);
  draw_string(text, MIN(text_len, prompt_width), r->x_zero + r->padding + ps1xlen, texty, r, PROMPT);

  XFillRectangle(r->d, r->w, r->completion_bg, start_at, r->y_zero, r->width, inner_height(r));

  for (size_t i = r->offset; i < cs->lenght; ++i) {
    struct completion *c = &cs->completions[i];

    enum text_type tt = cs->selected == (ssize_t)i ? COMPL_HIGH : COMPL;
    GC h = cs->selected == (ssize_t)i ? r->completion_highlighted_bg : r->completion_bg;

    int len = strlen(c->completion);
    int text_width = text_extents(c->completion, len, r, nil, nil);

    XFillRectangle(r->d, r->w, h, start_at, r->y_zero, text_width + r->padding*2, inner_height(r));

    draw_string(c->completion, len, start_at + r->padding, texty, r, tt);

    start_at += text_width + r->padding * 2;

    if (start_at > inner_width(r))
      break; // don't draw completion if the space isn't enough
  }
}

// |-----------------------------------------------------------------|
// |  prompt                                                         |
// |-----------------------------------------------------------------|
// |  completion                                                     |
// |-----------------------------------------------------------------|
// |  completion                                                     |
// |-----------------------------------------------------------------|
void draw_vertically(struct rendering *r, char *text, struct completions *cs) {
  int height, width;
  text_extents("fjpgl", 5, r, nil, &height);
  int start_at = r->padding*2 + height;

  XFillRectangle(r->d, r->w, r->completion_bg, r->x_zero, r->y_zero, r->width, r->height);
  XFillRectangle(r->d, r->w, r->prompt_bg, r->x_zero, r->y_zero, r->width, start_at);

  char *ps1dup = strdupn(r->ps1);
  int ps1xlen = text_extents(ps1dup != nil ? ps1dup : r->ps1, r->ps1len, r, nil, nil);
  free(ps1dup);

  draw_string(r->ps1, r->ps1len, r->x_zero + r->padding, r->y_zero + height + r->padding, r, PROMPT);
  draw_string(text, strlen(text), r->x_zero + r->padding + ps1xlen, r->y_zero + height + r->padding, r, PROMPT);

  start_at += r->y_zero;

  for (size_t i = r->offset; i < cs->lenght; ++i){
    struct completion *c = &cs->completions[i];
    enum text_type tt = cs->selected == (ssize_t)i ? COMPL_HIGH : COMPL;
    GC h = cs->selected == (ssize_t)i ? r->completion_highlighted_bg : r->completion_bg;

    int len = strlen(c->completion);
    text_extents(c->completion, len, r, &width, &height);
    XFillRectangle(r->d, r->w, h, r->x_zero, start_at, inner_width(r), height + r->padding*2);
    draw_string(c->completion, len, r->x_zero + r->padding, start_at + height + r->padding, r, tt);

    start_at += height + r->padding *2;

    if (start_at > inner_height(r))
      break; // don't draw completion if the space isn't enough
  }
}

void draw(struct rendering *r, char *text, struct completions *cs) {
  if (r->horizontal_layout)
    draw_horizontally(r, text, cs);
  else
    draw_vertically(r, text, cs);

  // draw the borders

  if (r->border_w != 0)
    XFillRectangle(r->d, r->w, r->border_w_bg, 0, 0, r->border_w, r->height);

  if (r->border_e != 0)
    XFillRectangle(r->d, r->w, r->border_e_bg, r->width - r->border_e, 0, r->border_e, r->height);

  if (r->border_n != 0)
    XFillRectangle(r->d, r->w, r->border_n_bg, 0, 0, r->width, r->border_n);

  if (r->border_s != 0)
    XFillRectangle(r->d, r->w, r->border_s_bg, 0, r->height - r->border_s, r->width, r->border_s);

  // send all the work to x
  XFlush(r->d);
}

/* Set some WM stuff */
void set_win_atoms_hints(Display *d, Window w, int width, int height) {
  Atom type;
  type = XInternAtom(d, "_NET_WM_WINDOW_TYPE_DOCK", false);
  XChangeProperty(
                  d,
                  w,
                  XInternAtom(d, "_NET_WM_WINDOW_TYPE", false),
                  XInternAtom(d, "ATOM", false),
                  32,
                  PropModeReplace,
                  (unsigned char *)&type,
                  1
                  );

  /* some window managers honor this properties */
  type = XInternAtom(d, "_NET_WM_STATE_ABOVE", false);
  XChangeProperty(d,
                  w,
                  XInternAtom(d, "_NET_WM_STATE", false),
                  XInternAtom(d, "ATOM", false),
                  32,
                  PropModeReplace,
                  (unsigned char *)&type,
                  1
                  );

  type = XInternAtom(d, "_NET_WM_STATE_FOCUSED", false);
  XChangeProperty(d,
                  w,
                  XInternAtom(d, "_NET_WM_STATE", false),
                  XInternAtom(d, "ATOM", false),
                  32,
                  PropModeAppend,
                  (unsigned char *)&type,
                  1
                  );

  // setting window hints
  XClassHint *class_hint = XAllocClassHint();
  if (class_hint == nil) {
    fprintf(stderr, "Could not allocate memory for class hint\n");
    exit(EX_UNAVAILABLE);
  }
  class_hint->res_name = resname;
  class_hint->res_class = resclass;
  XSetClassHint(d, w, class_hint);
  XFree(class_hint);

  XSizeHints *size_hint = XAllocSizeHints();
  if (size_hint == nil) {
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

// write the width and height of the window `w' respectively in `width'
// and `height'.
void get_wh(Display *d, Window *w, int *width, int *height) {
  XWindowAttributes win_attr;
  XGetWindowAttributes(d, *w, &win_attr);
  *height = win_attr.height;
  *width = win_attr.width;
}

int grabfocus(Display *d, Window w) {
  for (int i = 0; i < 100; ++i) {
    Window focuswin;
    int revert_to_win;
    XGetInputFocus(d, &focuswin, &revert_to_win);
    if (focuswin == w)
      return true;
    XSetInputFocus(d, w, RevertToParent, CurrentTime);
    usleep(1000);
  }
  return 0;
}

// I know this may seem a little hackish BUT is the only way I managed
// to actually grab that goddam keyboard. Only one call to
// XGrabKeyboard does not always end up with the keyboard grabbed!
int take_keyboard(Display *d, Window w) {
  int i;
  for (i = 0; i < 100; i++) {
    if (XGrabKeyboard(d, w, True, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess)
      return 1;
    usleep(1000);
  }
  fprintf(stderr, "Cannot grab keyboard\n");
  return 0;
}

// release the keyboard.
void release_keyboard(Display *d) {
  XUngrabKeyboard(d, CurrentTime);
}

unsigned long parse_color(const char *str, const char *def) {
  if (str == nil)
    goto invc;

  size_t len = strlen(str);

  // +1 for the '#' at the start, hence 9 and 4 (instead of 8 and 3)
  if (*str != '#' || len > 9 || len < 4)
    goto invc;
  ++str; // skip the '#'

  char *ep;
  errno = 0;
  rgba_t tmp = (rgba_t)(uint32_t)strtoul(str, &ep, 16);

  if (errno)
    goto invc;

  switch (len-1) {
  case 3:
    // expand: #rgb -> #rrggbb
    tmp.v = (tmp.v & 0xf00) * 0x1100
          | (tmp.v & 0x0f0) * 0x0110
          | (tmp.v & 0x00f) * 0x0011;
  case 6:
    // assume it has 100% opacity
    tmp.a = 0xff;
    break;
  } // colors in aarrggbb format needs no fixes

  // premultiply the alpha
  if (tmp.a) {
    tmp.r = (tmp.r * tmp.a) / 255;
    tmp.g = (tmp.g * tmp.a) / 255;
    tmp.b = (tmp.b * tmp.a) / 255;
    return tmp.v;
  }

  return 0U;

invc:
  fprintf(stderr, "Invalid color: \"%s\".\n", str);
  if (def != nil)
    return parse_color(def, nil);
  else
    return 0U;
}

// Given a string, try to parse it as a number or return
// `default_value'.
int parse_integer(const char *str, int default_value) {
  errno = 0;
  char *ep;
  long lval = strtol(str, &ep, 10);
  if (str[0] == '\0' || *ep != '\0') { // NaN
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

// like parse_integer, but if the value ends with a `%' then its
// treated like a percentage (`max' is used to compute the percentage)
int parse_int_with_percentage(const char *str, int default_value, int max) {
  int len = strlen(str);
  if (len > 0 && str[len-1] == '%') {
    char *cpy = strdup(str);
    check_allocation(cpy);
    cpy[len-1] = '\0';
    int val = parse_integer(cpy, default_value);
    free(cpy);
    return val * max / 100;
  }
  return parse_integer(str, default_value);
}

// like parse_int_with_percentage but understands some special values
// - "middle" that is (max - self) / 2
// - "start"  that is 0
// - "end"    that is (max - self)
int parse_int_with_pos(const char *str, int default_value, int max, int self) {
  if (!strcmp(str, "start"))
    return 0;
  if (!strcmp(str, "middle"))
    return (max - self)/2;
  if (!strcmp(str, "end"))
    return max-self;
  return parse_int_with_percentage(str, default_value, max);
}

// parse a string like a css value (for example like the css
// margin/padding properties). Will ALWAYS return an array of 4 word
// TODO: harden this function!
char **parse_csslike(const char *str) {
  char *s = strdup(str);
  if (s == nil)
    return nil;

  char **ret = malloc(4 * sizeof(char*));
  if (ret == nil) {
    free(s);
    return nil;
  }

  int i = 0;
  char *token;
  while ((token = strsep(&s, " ")) != NULL && i < 4) {
    ret[i] = strdup(token);
    i++;
  }

  if (i == 1)
    for (int j = 1; j < 4; j++)
      ret[j] = strdup(ret[0]);

  if (i == 2) {
    ret[2] = strdup(ret[0]);
    ret[3] = strdup(ret[1]);
  }

  if (i == 3)
    ret[3] = strdup(ret[1]);

  // Before we didn't check for the return type of strdup, here we will

  bool any_null = false;
  for (int i = 0; i < 4; ++i)
    any_null = ret[i] == nil || any_null;

  if (any_null)
    for (int i = 0; i < 4; ++i)
      if (ret[i] != nil)
        free(ret[i]);

  if (i == 0 || any_null) {
    free(s);
    free(ret);
    return nil;
  }

  return ret;
}

// Given an event, try to understand what the user wants. If the
// return value is ADD_CHAR then `input' is a pointer to a string that
// will need to be free'ed.
enum action parse_event(Display *d, XKeyPressedEvent *ev, XIC xic, char **input) {
  if (ev->keycode == XKeysymToKeycode(d, XK_BackSpace))
    return DEL_CHAR;

  if (ev->keycode == XKeysymToKeycode(d, XK_Tab))
    return ev->state & ShiftMask ? PREV_COMPL : NEXT_COMPL;

  if (ev->keycode == XKeysymToKeycode(d, XK_Return))
    return CONFIRM;

  if (ev->keycode == XKeysymToKeycode(d, XK_Escape))
    return EXIT;

  // try to read what the user pressed
  char str[SYM_BUF_SIZE] = {0};
  Status s = 0;
  Xutf8LookupString(xic, ev, str, SYM_BUF_SIZE, 0, &s);
  if (s == XBufferOverflow) {
    // should not happen since there are no utf-8 characters larger
    // than 24bits
    fprintf(stderr, "Buffer overflow when trying to create keyboard symbol map.\n");
    return EXIT;
  }

  if (ev->state & ControlMask) {
    if (!strcmp(str, "")) // C-u
      return DEL_LINE;
    if (!strcmp(str, "")) // C-w
      return DEL_WORD;
    if (!strcmp(str, "")) // C-h
      return DEL_CHAR;
    if (!strcmp(str, "\r")) // C-m
      return CONFIRM_CONTINUE;
    if (!strcmp(str, "")) // C-p
      return PREV_COMPL;
    if (!strcmp(str, "")) // C-n
      return NEXT_COMPL;
    if (!strcmp(str, "")) // C-c
      return EXIT;
    if (!strcmp(str, "\t")) // C-i
      return TOGGLE_FIRST_SELECTED;
  }

  *input = strdup(str);
  if (*input == nil) {
    fprintf(stderr, "Error while allocating memory for key.\n");
    return EXIT;
  }

  return ADD_CHAR;
}

// Given the name of the program (argv[0]?) print a small help on stderr
void usage(char *prgname) {
  fprintf(stderr, "%s [-Aamvh] [-B colors] [-b borders] [-C color] [-c color]\n"
                  "       [-d separator] [-e window] [-f font] [-H height] [-l layout]\n"
                  "       [-P padding] [-p prompt] [-T color] [-t color] [-S color]\n"
                  "       [-s color] [-W width] [-x coord] [-y coord]\n", prgname);
}

// small function used in the event loop
void confirm(enum state *status, struct rendering *r, struct completions *cs, char **text, int *textlen) {
  if ((cs->selected != -1) || (cs->lenght > 0 && r->first_selected)) {
    // if there is something selected expand it and return
    int index = cs->selected == -1 ? 0 : cs->selected;
    struct completion *c = cs->completions;
    while (true) {
      if (index == 0)
        break;
      c++;
      index--;
    }
    char *t = c->rcompletion;
    free(*text);
    *text = strdup(t);
    if (*text == nil) {
      fprintf(stderr, "Memory allocation error\n");
      *status = ERR;
    }
    *textlen = strlen(*text);
  } else {
    if (!r->free_text) {
      // cannot accept arbitrary text
      *status = LOOPING;
    }
  }
}

// event loop
enum state loop(struct rendering *r, char **text, int *textlen, struct completions *cs, char **lines, char **vlines) {
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
        // re-grab focus
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
            complete(cs, r->first_selected, true, text, textlen, &status);
            r->offset = cs->selected;
            break;
          }

          case NEXT_COMPL: {
            complete(cs, r->first_selected, false, text, textlen, &status);
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
            for (int i = 0; i < *textlen; ++i)
              *(*text + i) = 0;
            update_completions(cs, *text, lines, vlines, r->first_selected);
            r->offset = 0;
            break;
          }

          case ADD_CHAR: {
            int str_len = strlen(input);

            // sometimes a strange key is pressed (i.e. ctrl alone),
            // so input will be empty. Don't need to update completion
            // in this case
            if (str_len == 0)
              break;

            for (int i = 0; i < str_len; ++i) {
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
          r->offset = MIN(r->offset + 1, cs->lenght - 1);

        break;
      }
    }

    draw(r, *text, cs);
  }

  return status;
}

int main(int argc, char **argv) {
#ifdef HAVE_PLEDGE
  // stdio & rpat: to read and write stdio/stdout
  // unix:         to connect to Xorg
  pledge("stdio rpath unix", "");
#endif

  char *sep = nil;

  // by default the first completion isn't selected
  bool first_selected = false;

  // our parent window
  char *parent_window_id = nil;

  // the user can input arbitrary text
  bool free_text = true;

  // the user can select multiple entries
  bool multiple_select = false;

  // first round of args parsing
  int ch;
  while ((ch = getopt(argc, argv, ARGS)) != -1) {
    switch (ch) {
      case 'h': // help
        usage(*argv);
        return 0;
      case 'v': // version
        fprintf(stderr, "%s version: %s\n", *argv, VERSION);
        return 0;
      case 'e': // embed
        parent_window_id = strdup(optarg);
        check_allocation(parent_window_id);
        break;
      case 'd': {
        sep = strdup(optarg);
        check_allocation(sep);
      }
      case 'A': {
        free_text = false;
        break;
      }
      case 'm': {
        multiple_select = true;
        break;
      }
      default:
        break;
    }
  }

  // read the lines from stdin
  char **lines = nil;
  char *buf = nil;
  size_t nlines = readlines(&lines, &buf);

  char **vlines = nil;
  if (sep != nil) {
    int l = strlen(sep);
    vlines = calloc(nlines, sizeof(char*));
    check_allocation(vlines);

    for (int i = 0; lines[i] != nil; i++) {
      char *t = strstr(lines[i], sep);
      if (t == nil)
        vlines[i] = lines[i];
      else
        vlines[i] = t + l;
    }
  }

  setlocale(LC_ALL, getenv("LANG"));

  enum state status = LOOPING;

  // where the monitor start (used only with xinerama)
  int offset_x = 0;
  int offset_y = 0;

  // width and height of the window
  int width = 400;
  int height = 20;

  // position on the screen
  int x = 0;
  int y = 0;

  // the default padding
  int padding = 10;

  // the default borders
  int border_n = 0;
  int border_e = 0;
  int border_s = 0;
  int border_w = 0;

  // the prompt. We duplicate the string so later is easy to free (in
  // the case the user provide its own prompt)
  char *ps1 = strdup("$ ");
  check_allocation(ps1);

  // same for the font name
  char *fontname = strdup(default_fontname);
  check_allocation(fontname);

  int textlen = 10;
  char *text = malloc(textlen * sizeof(char));
  check_allocation(text);

  /* struct completions *cs = filter(text, lines); */
  struct completions *cs = compls_new(nlines);
  check_allocation(cs);

  // start talking to xorg
  Display *d = XOpenDisplay(nil);
  if (d == nil) {
    fprintf(stderr, "Could not open display!\n");
    return EX_UNAVAILABLE;
  }

  Window parent_window;
  bool embed = true;
  if (! (parent_window_id && (parent_window = strtol(parent_window_id, nil, 0)))) {
    parent_window = DefaultRootWindow(d);
    embed = false;
  }

  // get display size
  int d_width;
  int d_height;
  get_wh(d, &parent_window, &d_width, &d_height);

#ifdef USE_XINERAMA
  if (!embed && XineramaIsActive(d)) {
    // find the mice
    int number_of_screens = XScreenCount(d);
    Window r;
    Window root;
    int root_x, root_y, win_x, win_y;
    unsigned int mask;
    bool res;
    for (int i = 0; i < number_of_screens; ++i) {
      root = XRootWindow(d, i);
      res = XQueryPointer(d, root, &r, &r, &root_x, &root_y, &win_x, &win_y, &mask);
      if (res) break;
    }
    if (!res) {
      fprintf(stderr, "No mouse found.\n");
      root_x = 0;
      root_y = 0;
    }

    // now find in which monitor the mice is on
    int monitors;
    XineramaScreenInfo *info = XineramaQueryScreens(d, &monitors);
    if (info) {
      for (int i = 0; i < monitors; ++i) {
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

  /* Colormap cmap = DefaultColormap(d, DefaultScreen(d)); */
  XVisualInfo vinfo;
  XMatchVisualInfo(d, DefaultScreen(d), 32, TrueColor, &vinfo);

  Colormap cmap;
  cmap = XCreateColormap(d, XDefaultRootWindow(d), vinfo.visual, AllocNone);

  unsigned long p_fg = parse_color("#fff", nil);
  unsigned long compl_fg = parse_color("#fff", nil);
  unsigned long compl_highlighted_fg = parse_color("#000", nil);

  unsigned long p_bg = parse_color("#000", nil);
  unsigned long compl_bg = parse_color("#000", nil);
  unsigned long compl_highlighted_bg = parse_color("#fff", nil);

  unsigned long border_n_bg, border_e_bg, border_s_bg, border_w_bg;
  border_n_bg = border_e_bg = border_s_bg = border_w_bg = parse_color("#000", nil);

  bool horizontal_layout = true;

  // read resource
  XrmInitialize();
  char *xrm = XResourceManagerString(d);
  XrmDatabase xdb = nil;
  if (xrm != nil) {
    xdb = XrmGetStringDatabase(xrm);
    XrmValue value;
    char *datatype[20];

    if (XrmGetResource(xdb, "MyMenu.font", "*", datatype, &value) == true) {
      free(fontname);
      fontname = strdup(value.addr);
      check_allocation(fontname);
    } else {
      fprintf(stderr, "no font defined, using %s\n", fontname);
    }

    if (XrmGetResource(xdb, "MyMenu.layout", "*", datatype, &value) == true)
      horizontal_layout = !strcmp(value.addr, "horizontal");
    else
      fprintf(stderr, "no layout defined, using horizontal\n");

    if (XrmGetResource(xdb, "MyMenu.prompt", "*", datatype, &value) == true) {
      free(ps1);
      ps1 = normalize_str(value.addr);
    } else {
      fprintf(stderr, "no prompt defined, using \"%s\" as default\n", ps1);
    }

    if (XrmGetResource(xdb, "MyMenu.width", "*", datatype, &value) == true)
      width = parse_int_with_percentage(value.addr, width, d_width);
    else
      fprintf(stderr, "no width defined, using %d\n", width);

    if (XrmGetResource(xdb, "MyMenu.height", "*", datatype, &value) == true)
      height = parse_int_with_percentage(value.addr, height, d_height);
    else
      fprintf(stderr, "no height defined, using %d\n", height);

    if (XrmGetResource(xdb, "MyMenu.x", "*", datatype, &value) == true)
      x = parse_int_with_pos(value.addr, x, d_width, width);
    else
      fprintf(stderr, "no x defined, using %d\n", x);

    if (XrmGetResource(xdb, "MyMenu.y", "*", datatype, &value) == true)
      y = parse_int_with_pos(value.addr, y, d_height, height);
    else
      fprintf(stderr, "no y defined, using %d\n", y);

    if (XrmGetResource(xdb, "MyMenu.padding", "*", datatype, &value) == true)
      padding = parse_integer(value.addr, padding);
    else
      fprintf(stderr, "no padding defined, using %d\n", padding);

    if (XrmGetResource(xdb, "MyMenu.border.size", "*", datatype, &value) == true) {
      char **borders = parse_csslike(value.addr);
      if (borders != nil) {
        border_n = parse_integer(borders[0], 0);
        border_e = parse_integer(borders[1], 0);
        border_s = parse_integer(borders[2], 0);
        border_w = parse_integer(borders[3], 0);
      } else {
        fprintf(stderr, "error while parsing MyMenu.border.size\n");
      }
    } else {
      fprintf(stderr, "no border defined, using 0.\n");
    }

    /* XColor tmp; */
    // TODO: tmp needs to be free'd after every allocation?

    // prompt
    if (XrmGetResource(xdb, "MyMenu.prompt.foreground", "*", datatype, &value) == true)
      p_fg = parse_color(value.addr, "#fff");

    if (XrmGetResource(xdb, "MyMenu.prompt.background", "*", datatype, &value) == true)
      p_bg = parse_color(value.addr, "#000");

    // completion
    if (XrmGetResource(xdb, "MyMenu.completion.foreground", "*", datatype, &value) == true)
      compl_fg = parse_color(value.addr, "#fff");

    if (XrmGetResource(xdb, "MyMenu.completion.background", "*", datatype, &value) == true)
      compl_bg = parse_color(value.addr, "#000");
    else
      compl_bg = parse_color("#000", nil);

    // completion highlighted
    if (XrmGetResource(xdb, "MyMenu.completion_highlighted.foreground", "*", datatype, &value) == true)
      compl_highlighted_fg = parse_color(value.addr, "#000");

    if (XrmGetResource(xdb, "MyMenu.completion_highlighted.background", "*", datatype, &value) == true)
      compl_highlighted_bg = parse_color(value.addr, "#fff");
    else
      compl_highlighted_bg = parse_color("#fff", nil);

    // border
    if (XrmGetResource(xdb, "MyMenu.border.color", "*", datatype, &value) == true) {
      char **colors = parse_csslike(value.addr);
      if (colors != nil) {
        border_n_bg = parse_color(colors[0], "#000");
        border_e_bg = parse_color(colors[1], "#000");
        border_s_bg = parse_color(colors[2], "#000");
        border_w_bg = parse_color(colors[3], "#000");
      } else {
        fprintf(stderr, "error while parsing MyMenu.border.color\n");
      }
    }
  }

  // second round of args parsing
  optind = 0; // reset the option index
  while ((ch = getopt(argc, argv, ARGS)) != -1) {
    switch (ch) {
      case 'a':
        first_selected = true;
        break;
      case 'A':
        // free_text -- this case was already catched
        break;
      case 'd':
        // separator -- this case was already catched
        break;
      case 'e':
        // (embedding mymenu) this case was already catched.
      case 'm':
        // (multiple selection) this case was already catched.
        break;
      case 'p': {
        char *newprompt = strdup(optarg);
        if (newprompt != nil) {
          free(ps1);
          ps1 = newprompt;
        }
        break;
      }
      case 'x':
        x = parse_int_with_pos(optarg, x, d_width, width);
        break;
      case 'y':
        y = parse_int_with_pos(optarg, y, d_height, height);
        break;
      case 'P':
        padding = parse_integer(optarg, padding);
        break;
      case 'l':
        horizontal_layout = !strcmp(optarg, "horizontal");
        break;
      case 'f': {
        char *newfont = strdup(optarg);
        if (newfont != nil) {
          free(fontname);
          fontname = newfont;
        }
        break;
      }
      case 'W':
        width = parse_int_with_percentage(optarg, width, d_width);
        break;
      case 'H':
        height = parse_int_with_percentage(optarg, height, d_height);
        break;
      case 'b': {
        char **borders = parse_csslike(optarg);
        if (borders != nil) {
          border_n = parse_integer(borders[0], 0);
          border_e = parse_integer(borders[1], 0);
          border_s = parse_integer(borders[2], 0);
          border_w = parse_integer(borders[3], 0);
        } else {
          fprintf(stderr, "Error parsing b option\n");
        }
        break;
      }
      case 'B': {
        char **colors = parse_csslike(optarg);
        if (colors != nil) {
          border_n_bg = parse_color(colors[0], "#000");
          border_e_bg = parse_color(colors[1], "#000");
          border_s_bg = parse_color(colors[2], "#000");
          border_w_bg = parse_color(colors[3], "#000");
        } else {
          fprintf(stderr, "error while parsing B option\n");
        }
        break;
      }
      case 't': {
        p_fg = parse_color(optarg, nil);
        break;
      }
      case 'T': {
        p_bg = parse_color(optarg, nil);
        break;
      }
      case 'c': {
        compl_fg = parse_color(optarg, nil);
        break;
      }
      case 'C': {
        compl_bg = parse_color(optarg, nil);
        break;
      }
      case 's': {
        compl_highlighted_fg = parse_color(optarg, nil);
        break;
      }
      case 'S': {
        compl_highlighted_bg = parse_color(optarg, nil);
        break;
      }
      default:
        fprintf(stderr, "Unrecognized option %c\n", ch);
        status = ERR;
        break;
    }
  }

  // since only now we know if the first should be selected, update
  // the completion here
  update_completions(cs, text, lines, vlines, first_selected);

  // load the font
#ifdef USE_XFT
  XftFont *font = XftFontOpenName(d, DefaultScreen(d), fontname);
#else
  char **missing_charset_list;
  int missing_charset_count;
  XFontSet font = XCreateFontSet(d, fontname, &missing_charset_list, &missing_charset_count, nil);
  if (font == nil) {
    fprintf(stderr, "Unable to load the font(s) %s\n", fontname);
    return EX_UNAVAILABLE;
  }
#endif

  // create the window
  XSetWindowAttributes attr;
  attr.colormap = cmap;
  attr.override_redirect = true;
  attr.border_pixel = 0;
  attr.background_pixel = 0x80808080;
  attr.event_mask = ExposureMask | KeyPressMask | VisibilityChangeMask;

  Window w = XCreateWindow(d,                                   // display
                           parent_window,                       // parent
                           x + offset_x, y + offset_y,          // x y
                           width, height,                       // w h
                           0,                                   // border width
                           vinfo.depth,                         // depth
                           InputOutput,                         // class
                           vinfo.visual,                        // visual
                           CWBorderPixel | CWBackPixel | CWColormap | CWEventMask | CWOverrideRedirect, // value mask
                           &attr);

  set_win_atoms_hints(d, w, width, height);

  // we want some events
  XSelectInput(d, w, StructureNotifyMask | KeyPressMask | KeymapStateMask | ButtonPressMask);
  XMapRaised(d, w);

  // if embed, listen for other events as well
  if (embed) {
    XSelectInput(d, parent_window, FocusChangeMask);
    Window *children, parent, root;
    unsigned int children_no;
    if (XQueryTree(d, parent_window, &root, &parent, &children, &children_no) && children) {
      for (unsigned int i = 0; i < children_no && children[i] != w; ++i)
        XSelectInput(d, children[i], FocusChangeMask);
      XFree(children);
    }
    grabfocus(d, w);
  }

  // grab keyboard
  take_keyboard(d, w);

  // Create some graphics contexts
  XGCValues values;
  /* values.font = font->fid; */

  struct rendering r = {
    .d                          = d,
    .w                          = w,
    .width                      = width,
    .height                     = height,
    .padding                    = padding,
    .x_zero                     = border_w,
    .y_zero                     = border_n,
    .offset                     = 0,
    .free_text                  = free_text,
    .first_selected             = first_selected,
    .multiple_select            = multiple_select,
    .border_n                   = border_n,
    .border_e                   = border_e,
    .border_s                   = border_s,
    .border_w                   = border_w,
    .horizontal_layout          = horizontal_layout,
    .ps1                        = ps1,
    .ps1len                     = strlen(ps1),
    .prompt                     = XCreateGC(d, w, 0, &values),
    .prompt_bg                  = XCreateGC(d, w, 0, &values),
    .completion                 = XCreateGC(d, w, 0, &values),
    .completion_bg              = XCreateGC(d, w, 0, &values),
    .completion_highlighted     = XCreateGC(d, w, 0, &values),
    .completion_highlighted_bg  = XCreateGC(d, w, 0, &values),
    .border_n_bg                = XCreateGC(d, w, 0, &values),
    .border_e_bg                = XCreateGC(d, w, 0, &values),
    .border_s_bg                = XCreateGC(d, w, 0, &values),
    .border_w_bg                = XCreateGC(d, w, 0, &values),
#ifdef USE_XFT
    .font                       = font,
#else
    .font                       = &font,
#endif
  };

#ifdef USE_XFT
  r.xftdraw = XftDrawCreate(d, w, vinfo.visual, DefaultColormap(d, 0));

  {
    rgba_t c;

    XRenderColor xrcolor;

    // prompt
    c = *(rgba_t*)&p_fg;
    xrcolor.red          = EXPANDBITS(c.r);
    xrcolor.green        = EXPANDBITS(c.g);
    xrcolor.blue         = EXPANDBITS(c.b);
    xrcolor.alpha        = EXPANDBITS(c.a);
    XftColorAllocValue(d, DefaultVisual(d, 0), DefaultColormap(d, 0), &xrcolor, &r.xft_prompt);

    // completion
    c = *(rgba_t*)&compl_fg;
    xrcolor.red          = EXPANDBITS(c.r);
    xrcolor.green        = EXPANDBITS(c.g);
    xrcolor.blue         = EXPANDBITS(c.b);
    xrcolor.alpha        = EXPANDBITS(c.a);
    XftColorAllocValue(d, DefaultVisual(d, 0), DefaultColormap(d, 0), &xrcolor, &r.xft_completion);

    // completion highlighted
    c = *(rgba_t*)&compl_highlighted_fg;
    xrcolor.red          = EXPANDBITS(c.r);
    xrcolor.green        = EXPANDBITS(c.g);
    xrcolor.blue         = EXPANDBITS(c.b);
    xrcolor.alpha        = EXPANDBITS(c.a);
    XftColorAllocValue(d, DefaultVisual(d, 0), DefaultColormap(d, 0), &xrcolor, &r.xft_completion_highlighted);
  }
#endif

  // load the colors in our GCs
  /* XSetForeground(d, r.prompt, p_fg.pixel); */
  XSetForeground(d, r.prompt, p_fg);
  XSetForeground(d, r.prompt_bg, p_bg);
  /* XSetForeground(d, r.completion, compl_fg.pixel); */
  XSetForeground(d, r.completion, compl_fg);
  XSetForeground(d, r.completion_bg, compl_bg);
  /* XSetForeground(d, r.completion_highlighted, compl_highlighted_fg.pixel); */
  XSetForeground(d, r.completion_highlighted, compl_highlighted_fg);
  XSetForeground(d, r.completion_highlighted_bg, compl_highlighted_bg);
  XSetForeground(d, r.border_n_bg, border_n_bg);
  XSetForeground(d, r.border_e_bg, border_e_bg);
  XSetForeground(d, r.border_s_bg, border_s_bg);
  XSetForeground(d, r.border_w_bg, border_w_bg);

  // open the X input method
  XIM xim = XOpenIM(d, xdb, resname, resclass);
  check_allocation(xim);

  XIMStyles *xis = nil;
  if (XGetIMValues(xim, XNQueryInputStyle, &xis, NULL) || !xis) {
    fprintf(stderr, "Input Styles could not be retrieved\n");
    return EX_UNAVAILABLE;
  }

  XIMStyle bestMatchStyle = 0;
  for (int i = 0; i < xis->count_styles; ++i) {
    XIMStyle ts = xis->supported_styles[i];
    if (ts == (XIMPreeditNothing | XIMStatusNothing)) {
      bestMatchStyle = ts;
      break;
    }
  }
  XFree(xis);

  if (!bestMatchStyle) {
    fprintf(stderr, "No matching input style could be determined\n");
  }

  r.xic = XCreateIC(xim, XNInputStyle, bestMatchStyle, XNClientWindow, w, XNFocusWindow, w, NULL);
  check_allocation(r.xic);

  // draw the window for the first time
  draw(&r, text, cs);

  // main loop
  while (status == LOOPING || status == OK_LOOP) {
    status = loop(&r, &text, &textlen, cs, lines, vlines);

    if (status != ERR)
      printf("%s\n", text);

    if (!multiple_select && status == OK_LOOP)
      status = OK;
  }

  release_keyboard(r.d);

#ifdef USE_XFT
  XftColorFree(r.d, DefaultVisual(r.d, 0), DefaultColormap(r.d, 0), &r.xft_prompt);
  XftColorFree(r.d, DefaultVisual(r.d, 0), DefaultColormap(r.d, 0), &r.xft_completion);
  XftColorFree(r.d, DefaultVisual(r.d, 0), DefaultColormap(r.d, 0), &r.xft_completion_highlighted);
#endif

  free(ps1);
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
