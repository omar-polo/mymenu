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

#define ARGS "hvae:p:P:l:f:W:H:x:y:b:B:t:T:c:C:s:S:"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

// If we don't have it or we don't want an "ignore case" completion
// style, fall back to `strstr(3)`
#ifndef USE_STRCASESTR
# define strcasestr strstr
#endif

#define INITIAL_ITEMS 64

#define check_allocation(a) {                         \
    if (a == nil) {                                   \
      fprintf(stderr, "Could not allocate memory\n"); \
      abort();                                        \
    }                                                 \
  }

#define inner_height(r) (r->height - r->border_n - r->border_s)
#define inner_width(r)  (r->width  - r->border_e - r->border_w)

// The possible state of the event loop.
enum state {LOOPING, OK, ERR};

// for the drawing-related function. The text to be rendered could be
// the prompt, a completion or a highlighted completion
enum text_type {PROMPT, COMPL, COMPL_HIGH};

// These are the possible action to be performed after user input.
enum action {
  EXIT,
  CONFIRM,
  NEXT_COMPL,
  PREV_COMPL,
  DEL_CHAR,
  DEL_WORD,
  DEL_LINE,
  ADD_CHAR,
  TOGGLE_FIRST_SELECTED
};

struct rendering {
  Display *d; // connection to xorg
  Window w;
  int width;
  int height;
  int padding;
  int x_zero; // the "zero" on the x axis (may not be 0 'cause the border)
  int y_zero; // the same a x_zero, only for the y axis

  // The four border
  int border_n;
  int border_e;
  int border_s;
  int border_w;

  bool horizontal_layout;

  // the prompt
  char *ps1;
  int ps1len;

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

// A simple linked list to store the completions.
struct completion {
  char *completion;
  struct completion *next;
};

struct completions {
  struct completion *completions;
  int selected;
  int lenght;
};

// return a newly allocated (and empty) completion list
struct completions *compls_new() {
  struct completions *cs = malloc(sizeof(struct completions));

  if (cs == nil)
    return cs;

  cs->completions = nil;
  cs->selected = -1;
  cs->lenght = 0;
  return cs;
}

struct completion *compl_new() {
  struct completion *c = malloc(sizeof(struct completion));
  if (c == nil)
    return c;

  c->completion = nil;
  c->next = nil;
  return c;
}

// delete ONLY the given completion (i.e. does not free c->next...)
void compl_delete(struct completion *c) {
  free(c);
}

// delete the current completion and the next (c->next) and so on...
void compl_delete_rec(struct completion *c) {
  while (c != nil) {
    struct completion *t = c->next;
    free(c);
    c = t;
  }
}

void compls_delete(struct completions *cs) {
  if (cs == nil)
    return;

  compl_delete_rec(cs->completions);
  free(cs);
}

// create a completion list from a text and the list of possible
// completions (null terminated). Expects a non-null `cs'.
void filter(struct completions *cs, char *text, char **lines) {
  struct completion *c = compl_new();
  if (c == nil) {
    return;
  }

  cs->completions = c;

  int index = 0;
  int matching = 0;

  while (true) {
    char *l = lines[index];
    if (l == nil)
      break;

    if (strcasestr(l, text) != nil) {
      matching++;

      c->next = compl_new();
      c = c->next;
      if (c == nil) {
        compls_delete(cs);
        return;
      }
      c->completion = l;
    }

    index++;
  }

  struct completion *f = cs->completions->next;
  compl_delete(cs->completions);
  cs->completions = f;
  cs->lenght = matching;
  cs->selected = -1;
}

// update the given completion, that is: clean the old cs & generate a new one.
void update_completions(struct completions *cs, char *text, char **lines, bool first_selected) {
  compl_delete_rec(cs->completions);
  filter(cs, text, lines);
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

  struct completion *n = cs->completions;

  // find the selected item
  while (index != 0) {
    index--;
    n = n->next;
  }

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

// return the number of character
int utf8strnlen(char *s, int maxlen) {
  int len = 0;
  while (*s && maxlen > 0) {
    len += (*s++ & 0xc0) != 0x80;
    maxlen--;
  }
  return len;
}

// remove the last *glyph* from the *utf8* string!
// this is different from just setting the last byte to 0 (in some
// cases ofc). The actual implementation is quite inefficient because
// it remove the last byte until the number of glyphs doesn't change
void popc(char *p, int maxlen) {
  int len = strnlen(p, maxlen);

  if (len == 0)
    return;

  int ulen = utf8strnlen(p, maxlen);
  while (len > 0 && utf8strnlen(p, maxlen) == ulen) {
    len--;
    p[len] = 0;
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

// read an arbitrary long line from stdin and return a pointer to it
// TODO: resize the allocated memory to exactly fit the string once
// read?
char *readline(bool *eof) {
  int maxlen = 8;
  char *str = calloc(maxlen, sizeof(char));
  if (str == nil) {
    fprintf(stderr, "Cannot allocate memory!\n");
    exit(EX_UNAVAILABLE);
  }

  int c;
  while((c = getchar()) != EOF) {
    if (c == '\n')
      return str;
    else
      maxlen = pushc(&str, maxlen, c);

    if (maxlen == -1) {
      fprintf(stderr, "Cannot allocate memory!\n");
      exit(EX_UNAVAILABLE);
    }
  }
  *eof = true;
  return str;
}

// read an arbitrary amount of text until an EOF and store it in
// lns. `items` is the capacity of lns. It may increase lns with
// `realloc(3)` to store more line. Return the number of lines
// read. The last item will always be a NULL pointer. It ignore the
// "null" (empty) lines
int readlines (char ***lns, int items) {
  bool finished = false;
  int n = 0;
  char **lines = *lns;
  while (true) {
    lines[n] = readline(&finished);

    if (strlen(lines[n]) == 0 || lines[n][0] == '\n') {
      free(lines[n]);
      --n; // forget about this line
    }

    if (finished)
      break;

    ++n;

    if (n == items - 1) {
      items += items >>1;
      char **l = realloc(lines, sizeof(char*) * items);
      check_allocation(l);
      *lns = l;
      lines = l;
    }
  }

  n++;
  lines[n] = nil;
  return items;
}

// Compute the dimension of the string str once rendered, return the
// width and save the width and the height in ret_width and ret_height
int text_extents(char *str, int len, struct rendering *r, int *ret_width, int *ret_height) {
  int height;
  int width;
#ifdef USE_XFT
  XGlyphInfo gi;
  XftTextExtentsUtf8(r->d, r->font, str, len, &gi);
  /* height = gi.height; */
  /* height = (gi.height + (r->font->ascent - r->font->descent)/2) / 2; */
  /* height = (r->font->ascent - r->font->descent)/2 + gi.height*2; */
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

  int width, height;
  int ps1xlen = text_extents(r->ps1, r->ps1len, r, &width, &height);
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

  struct completion *c = cs->completions;
  for (int i = 0; c != nil; ++i) {
    enum text_type tt = cs->selected == i ? COMPL_HIGH : COMPL;
    GC h = cs->selected == i ? r->completion_highlighted_bg : r->completion_bg;

    int len = strlen(c->completion);
    int text_width = text_extents(c->completion, len, r, nil, nil);

    XFillRectangle(r->d, r->w, h, start_at, r->y_zero, text_width + r->padding*2, inner_height(r));

    draw_string(c->completion, len, start_at + r->padding, texty, r, tt);

    start_at += text_width + r->padding * 2;

    if (start_at > inner_width(r))
      break; // don't draw completion if the space isn't enough

    c = c->next;
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
  int start_at = height + r->padding;

  XFillRectangle(r->d, r->w, r->completion_bg, r->x_zero, r->y_zero, r->width, r->height);
  XFillRectangle(r->d, r->w, r->prompt_bg, r->x_zero, r->y_zero, r->width, start_at);

  int ps1xlen = text_extents(r->ps1, r->ps1len, r, nil, nil);

  draw_string(r->ps1, r->ps1len, r->x_zero + r->padding, r->y_zero + height + r->padding, r, PROMPT);
  draw_string(text, strlen(text), r->x_zero + r->padding + ps1xlen, r->y_zero + height + r->padding, r, PROMPT);

  start_at += r->padding + r->y_zero;

  struct completion *c = cs->completions;
  for (int i = 0; c != nil; ++i){
    enum text_type tt = cs->selected == i ? COMPL_HIGH : COMPL;
    GC h = cs->selected == i ? r->completion_highlighted_bg : r->completion_bg;

    int len = strlen(c->completion);
    text_extents(c->completion, len, r, &width, &height);
    XFillRectangle(r->d, r->w, h, r->x_zero, start_at, inner_width(r), height + r->padding*2);
    draw_string(c->completion, len, r->x_zero + r->padding, start_at + height + r->padding, r, tt);

    start_at += height + r->padding *2;

    if (start_at > inner_height(r))
      break; // don't draw completion if the space isn't enough

    c = c->next;
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
      return CONFIRM;
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
  fprintf(stderr, "%s [-hva] [-p prompt] [-x coord] [-y coord] [-W width] [-H height]\n"
                  "       [-P padding] [-l layout] [-f font] [-b borders] [-B colors]\n"
                  "       [-t color] [-T color] [-c color] [-C color] [-s color] [-S color]\n"
                  "       [-w window_id]\n", prgname);
}

int main(int argc, char **argv) {
#ifdef HAVE_PLEDGE
  // stdio & rpat: to read and write stdio/stdout
  // unix:         to connect to Xorg
  pledge("stdio rpath unix", "");
#endif

  // by default the first completion isn't selected
  bool first_selected = false;

  char *parent_window_id = nil;

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
      default:
        break;
    }
  }

  char **lines = calloc(INITIAL_ITEMS, sizeof(char*));
  readlines(&lines, INITIAL_ITEMS);

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
  struct completions *cs = compls_new();
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

  fprintf(stderr, "d_width %d, d_height %d\n", d_width, d_height);

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

  Colormap cmap = DefaultColormap(d, DefaultScreen(d));
  XColor p_fg, p_bg,
    compl_fg, compl_bg,
    compl_highlighted_fg, compl_highlighted_bg,
    border_n_bg, border_e_bg, border_s_bg, border_w_bg;

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
      fontname = strdup(value.addr);
      check_allocation(fontname);
    }
    else
      fprintf(stderr, "no font defined, using %s\n", fontname);

    if (XrmGetResource(xdb, "MyMenu.layout", "*", datatype, &value) == true) {
      horizontal_layout = !strcmp(value.addr, "horizontal");
    }
    else
      fprintf(stderr, "no layout defined, using horizontal\n");

    if (XrmGetResource(xdb, "MyMenu.prompt", "*", datatype, &value) == true) {
      free(ps1);
      ps1 = normalize_str(value.addr);
    } else
      fprintf(stderr, "no prompt defined, using \"%s\" as default\n", ps1);

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

    XColor tmp;
    // TODO: tmp needs to be free'd after every allocation?

    // prompt
    if (XrmGetResource(xdb, "MyMenu.prompt.foreground", "*", datatype, &value) == true)
      XAllocNamedColor(d, cmap, value.addr, &p_fg, &tmp);
    else
      XAllocNamedColor(d, cmap, "white", &p_fg, &tmp);

    if (XrmGetResource(xdb, "MyMenu.prompt.background", "*", datatype, &value) == true)
      XAllocNamedColor(d, cmap, value.addr, &p_bg, &tmp);
    else
      XAllocNamedColor(d, cmap, "black", &p_bg, &tmp);

    // completion
    if (XrmGetResource(xdb, "MyMenu.completion.foreground", "*", datatype, &value) == true)
      XAllocNamedColor(d, cmap, value.addr, &compl_fg, &tmp);
    else
      XAllocNamedColor(d, cmap, "white", &compl_fg, &tmp);

    if (XrmGetResource(xdb, "MyMenu.completion.background", "*", datatype, &value) == true)
      XAllocNamedColor(d, cmap, value.addr, &compl_bg, &tmp);
    else
      XAllocNamedColor(d, cmap, "black", &compl_bg, &tmp);

    // completion highlighted
    if (XrmGetResource(xdb, "MyMenu.completion_highlighted.foreground", "*", datatype, &value) == true)
      XAllocNamedColor(d, cmap, value.addr, &compl_highlighted_fg, &tmp);
    else
      XAllocNamedColor(d, cmap, "black", &compl_highlighted_fg, &tmp);

    if (XrmGetResource(xdb, "MyMenu.completion_highlighted.background", "*", datatype, &value) == true)
      XAllocNamedColor(d, cmap, value.addr, &compl_highlighted_bg, &tmp);
    else
      XAllocNamedColor(d, cmap, "white", &compl_highlighted_bg, &tmp);

    // border
    if (XrmGetResource(xdb, "MyMenu.border.color", "*", datatype, &value) == true) {
      char **colors = parse_csslike(value.addr);
      if (colors != nil) {
        XAllocNamedColor(d, cmap, colors[0], &border_n_bg, &tmp);
        XAllocNamedColor(d, cmap, colors[1], &border_e_bg, &tmp);
        XAllocNamedColor(d, cmap, colors[2], &border_s_bg, &tmp);
        XAllocNamedColor(d, cmap, colors[3], &border_w_bg, &tmp);
      } else {
        fprintf(stderr, "error while parsing MyMenu.border.color\n");
      }
    } else {
      XAllocNamedColor(d, cmap, "white", &border_n_bg, &tmp);
      XAllocNamedColor(d, cmap, "white", &border_e_bg, &tmp);
      XAllocNamedColor(d, cmap, "white", &border_s_bg, &tmp);
      XAllocNamedColor(d, cmap, "white", &border_w_bg, &tmp);
    }
  } else {
    XColor tmp;
    XAllocNamedColor(d, cmap, "white", &p_fg, &tmp);
    XAllocNamedColor(d, cmap, "black", &p_bg, &tmp);
    XAllocNamedColor(d, cmap, "white", &compl_fg, &tmp);
    XAllocNamedColor(d, cmap, "black", &compl_bg, &tmp);
    XAllocNamedColor(d, cmap, "black", &compl_highlighted_fg, &tmp);
    XAllocNamedColor(d, cmap, "white", &border_n_bg, &tmp);
    XAllocNamedColor(d, cmap, "white", &border_e_bg, &tmp);
    XAllocNamedColor(d, cmap, "white", &border_s_bg, &tmp);
    XAllocNamedColor(d, cmap, "white", &border_w_bg, &tmp);
  }

  // second round of args parsing
  optind = 0; // reset the option index
  while ((ch = getopt(argc, argv, ARGS)) != -1) {
    switch (ch) {
      case 'a':
        first_selected = true;
        break;
      case 'e':
        // (embedding mymenu) this case was already catched.
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
          XColor tmp;
          XAllocNamedColor(d, cmap, colors[0], &border_n_bg, &tmp);
          XAllocNamedColor(d, cmap, colors[1], &border_e_bg, &tmp);
          XAllocNamedColor(d, cmap, colors[2], &border_s_bg, &tmp);
          XAllocNamedColor(d, cmap, colors[3], &border_w_bg, &tmp);
        } else {
          fprintf(stderr, "error while parsing B option\n");
        }
        break;
      }
      case 't': {
        XColor tmp;
        XAllocNamedColor(d, cmap, optarg, &p_fg, &tmp);
        break;
      }
      case 'T': {
        XColor tmp;
        XAllocNamedColor(d, cmap, optarg, &p_bg, &tmp);
        break;
      }
      case 'c': {
        XColor tmp;
        XAllocNamedColor(d, cmap, optarg, &compl_fg, &tmp);
        break;
      }
      case 'C': {
        XColor tmp;
        XAllocNamedColor(d, cmap, optarg, &compl_bg, &tmp);
        break;
      }
      case 's': {
        XColor tmp;
        XAllocNamedColor(d, cmap, optarg, &compl_highlighted_fg, &tmp);
        break;
      }
      case 'S': {
        XColor tmp;
        XAllocNamedColor(d, cmap, optarg, &compl_highlighted_bg, &tmp);
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
  update_completions(cs, text, lines, first_selected);

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
  attr.override_redirect = true;
  attr.event_mask = ExposureMask | KeyPressMask | VisibilityChangeMask;

  Window w = XCreateWindow(d,                                   // display
                           parent_window,                       // parent
                           x + offset_x, y + offset_y,          // x y
                           width, height,                       // w h
                           0,                                   // border width
                           CopyFromParent,                      // depth
                           InputOutput,                         // class
                           CopyFromParent,                      // visual
                           CWEventMask | CWOverrideRedirect,    // value mask (CWBackPixel in the future also?)
                           &attr);

  set_win_atoms_hints(d, w, width, height);

  // we want some events
  XSelectInput(d, w, StructureNotifyMask | KeyPressMask | KeymapStateMask);
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
  r.xftdraw = XftDrawCreate(d, w, DefaultVisual(d, 0), DefaultColormap(d, 0));

  // prompt
  XRenderColor xrcolor;
  xrcolor.red          = p_fg.red;
  xrcolor.green        = p_fg.red;
  xrcolor.blue         = p_fg.red;
  xrcolor.alpha        = 65535;
  XftColorAllocValue(d, DefaultVisual(d, 0), DefaultColormap(d, 0), &xrcolor, &r.xft_prompt);

  // completion
  xrcolor.red          = compl_fg.red;
  xrcolor.green        = compl_fg.green;
  xrcolor.blue         = compl_fg.blue;
  xrcolor.alpha        = 65535;
  XftColorAllocValue(d, DefaultVisual(d, 0), DefaultColormap(d, 0), &xrcolor, &r.xft_completion);

  // completion highlighted
  xrcolor.red          = compl_highlighted_fg.red;
  xrcolor.green        = compl_highlighted_fg.green;
  xrcolor.blue         = compl_highlighted_fg.blue;
  xrcolor.alpha        = 65535;
  XftColorAllocValue(d, DefaultVisual(d, 0), DefaultColormap(d, 0), &xrcolor, &r.xft_completion_highlighted);
#endif

  // load the colors in our GCs
  XSetForeground(d, r.prompt, p_fg.pixel);
  XSetForeground(d, r.prompt_bg, p_bg.pixel);
  XSetForeground(d, r.completion, compl_fg.pixel);
  XSetForeground(d, r.completion_bg, compl_bg.pixel);
  XSetForeground(d, r.completion_highlighted, compl_highlighted_fg.pixel);
  XSetForeground(d, r.completion_highlighted_bg, compl_highlighted_bg.pixel);
  XSetForeground(d, r.border_n_bg, border_n_bg.pixel);
  XSetForeground(d, r.border_e_bg, border_e_bg.pixel);
  XSetForeground(d, r.border_s_bg, border_s_bg.pixel);
  XSetForeground(d, r.border_w_bg, border_w_bg.pixel);

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

  XIC xic = XCreateIC(xim, XNInputStyle, bestMatchStyle, XNClientWindow, w, XNFocusWindow, w, NULL);
  check_allocation(xic);

  // draw the window for the first time
  draw(&r, text, cs);

  // main loop
  while (status == LOOPING) {
    XEvent e;
    XNextEvent(d, &e);

    if (XFilterEvent(&e, w))
      continue;

    switch (e.type) {
      case KeymapNotify:
        XRefreshKeyboardMapping(&e.xmapping);
        break;

      case FocusIn:
        // re-grab focus
        if (e.xfocus.window != w)
          grabfocus(d, w);
        break;

      case VisibilityNotify:
        if (e.xvisibility.state != VisibilityUnobscured)
          XRaiseWindow(d, w);
        break;

      case MapNotify:
        /* fprintf(stderr, "Map Notify!\n"); */
        /* TODO: update the computed window and height! */
        /* get_wh(d, &w, width, height); */
        draw(&r, text, cs);
        break;

      case KeyPress: {
        XKeyPressedEvent *ev = (XKeyPressedEvent*)&e;

        char *input;
        switch (parse_event(d, ev, xic, &input)) {
          case EXIT:
            status = ERR;
            break;

          case CONFIRM:
            status = OK;

            // if first_selected is active and the first completion is
            // active be sure to 'expand' the text to match the selection
            if (first_selected && cs && cs->selected == 0) {
              free(text);
              text = strdup(cs->completions->completion);
              if (text == nil) {
                fprintf(stderr, "Memory allocation error");
                status = ERR;
              }
              textlen = strlen(text);
            }
            break;

          case PREV_COMPL: {
            complete(cs, first_selected, true, &text, &textlen, &status);
            break;
          }

          case NEXT_COMPL: {
            complete(cs, first_selected, false, &text, &textlen, &status);
            break;
          }

          case DEL_CHAR:
            popc(text, textlen);
            update_completions(cs, text, lines, first_selected);
            break;

          case DEL_WORD: {
            // `textlen` is the lenght of the allocated string, not the
            // lenght of the ACTUAL string
            int p = strlen(text) -1;
            if (p > 0) { // delete the current char
              text[p] = 0;
              p--;
            }

            // erase the alphanumeric char
            while (p >= 0 && isalnum(text[p])) {
              text[p] = 0;
              p--;
            }

            // erase also trailing white spaces
            while (p >= 0 && isspace(text[p])) {
              text[p] = 0;
              p--;
            }
            update_completions(cs, text, lines, first_selected);
            break;
          }

          case DEL_LINE: {
            for (int i = 0; i < textlen; ++i)
              text[i] = 0;
            update_completions(cs, text, lines, first_selected);
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
              textlen = pushc(&text, textlen, input[i]);
              if (textlen == -1) {
                fprintf(stderr, "Memory allocation error\n");
                status = ERR;
                break;
              }
            }
            if (status != ERR) {
              update_completions(cs, text, lines, first_selected);
              free(input);
            }
            break;
          }

          case TOGGLE_FIRST_SELECTED:
            first_selected = !first_selected;
            if (first_selected && cs->selected < 0)
              cs->selected = 0;
            if (!first_selected && cs->selected == 0)
              cs->selected = -1;
            break;
        }
      }
    }

    draw(&r, text, cs);
  }

  if (status == OK)
    printf("%s\n", text);

  release_keyboard(r.d);

#ifdef USE_XFT
  XftColorFree(r.d, DefaultVisual(r.d, 0), DefaultColormap(r.d, 0), &r.xft_prompt);
  XftColorFree(r.d, DefaultVisual(r.d, 0), DefaultColormap(r.d, 0), &r.xft_completion);
  XftColorFree(r.d, DefaultVisual(r.d, 0), DefaultColormap(r.d, 0), &r.xft_completion_highlighted);
#endif

  free(ps1);
  free(fontname);
  free(text);

  char *l = nil;
  char **lns = lines;
  while ((l = *lns) != nil) {
    free(l);
    ++lns;
  }

  free(lines);
  compls_delete(cs);

  XDestroyWindow(r.d, r.w);
  XCloseDisplay(r.d);

  return status;
}
