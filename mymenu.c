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

#define nil NULL
#define resname "MyMenu"
#define resclass "mymenu"

#ifdef USE_XFT
# define default_fontname "monospace"
#else
# define default_fontname "fixed"
#endif

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

// If we don't have it or we don't want an "ignore case" completion
// style, fall back to `strstr(3)`
#ifndef USE_STRCASESTR
#define strcasestr strstr
#endif

#define update_completions(cs, text, lines) {   \
    compl_delete(cs);                           \
    cs = filter(text, lines);                   \
  }

#define complete(cs, nothing_selected, p, text, textlen, status) {      \
    struct completions *n = p                                           \
      ? compl_select_prev(cs, nothing_selected)                         \
      : compl_select_next(cs, nothing_selected);                        \
                                                                        \
    if (n != nil) {                                                     \
      nothing_selected = false;                                         \
      free(text);                                                       \
      text = strdup(n->completion);                                     \
      if (text == nil) {                                                \
        fprintf(stderr, "Memory allocation error!\n");                  \
        status = ERR;                                                   \
        break;                                                          \
      }                                                                 \
      textlen = strlen(text);                                           \
    }                                                                   \
  }

#define INITIAL_ITEMS 64

#define cannot_allocate_memory {                        \
    fprintf(stderr, "Could not allocate memory\n");     \
    abort();                                            \
  }

#define check_allocation(a) {     \
    if (a == nil)                 \
      cannot_allocate_memory;     \
  }

enum state {LOOPING, OK, ERR};

enum text_type {PROMPT, COMPL, COMPL_HIGH};

struct rendering {
  Display *d;
  Window w;
  GC prompt;
  GC prompt_bg;
  GC completion;
  GC completion_bg;
  GC completion_highlighted;
  GC completion_highlighted_bg;
#ifdef USE_XFT
  XftDraw *xftdraw;
  XftColor xft_prompt;
  XftColor xft_completion;
  XftColor xft_completion_highlighted;
  XftFont *font;
#else
  XFontSet *font;
#endif
  int width;
  int height;
  bool horizontal_layout;
  char *ps1;
  int ps1len;
};

struct completions {
  char *completion;
  bool selected;
  struct completions *next;
};

struct completions *compl_new() {
  struct completions *c = malloc(sizeof(struct completions));

  if (c == nil)
    return c;

  c->completion = nil;
  c->selected = false;
  c->next = nil;
  return c;
}

void compl_delete(struct completions *c) {
  free(c);
}

struct completions *compl_select_next(struct completions *c, bool n) {
  if (c == nil)
    return nil;
  if (n) {
    c->selected = true;
    return c;
  }

  struct completions *orig = c;
  while (c != nil) {
    if (c->selected) {
      c->selected = false;
      if (c->next != nil) {
        // the current one is selected and the next one exists
        c->next->selected = true;
        return c->next;
      } else {
        // the current one is selected and the next one is nill,
        // select the first one
        orig->selected = true;
        return orig;
      }
    }
    c = c->next;
  }
  return nil;
}

struct completions *compl_select_prev(struct completions *c, bool n) {
  if (c == nil)
    return nil;

  struct completions *cc = c;

  if (n) // select the last one
    while (cc != nil) {
      if (cc->next == nil) {
        cc->selected = true;
        return cc;
      }
      cc = cc->next;
    }
  else // select the previous one
    while (cc != nil) {
      if (cc->next != nil && cc->next->selected) {
        cc->next->selected = false;
        cc->selected = true;
        return cc;
      }
      cc = cc->next;
    }

  return nil;
}

struct completions *filter(char *text, char **lines) {
  int i = 0;
  struct completions *root = compl_new();
  struct completions *c = root;

  for (;;) {
    char *l = lines[i];
    if (l == nil)
      break;

    if (strcasestr(l, text) != nil) {
      c->next = compl_new();
      c = c->next;
      c->completion = l;
    }

    ++i;
  }

  struct completions *r = root->next;
  compl_delete(root);
  return r;
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
// it remove the last char until the number of glyphs doesn't change
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
  // Honestly I don't know why this won't work, but I found that this
  // formula seems to work with various ttf font
  /* height = gi.height; */
  height = (gi.height + (r->font->ascent - r->font->descent)/2) / 2;
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
  // TODO: make these dynamic?
  int prompt_width = 20; // char
  int padding = 10;

  int width, height;
  char *ps1_dup = strdupn(r->ps1);
  ps1_dup = ps1_dup == nil ? r->ps1 : ps1_dup;
  int ps1xlen = text_extents(ps1_dup, r->ps1len, r, &width, &height);
  free(ps1_dup);
  int start_at = ps1xlen;

  start_at = text_extents("n", 1, r, nil, nil);
  start_at = start_at * prompt_width + padding;

  int texty = (height + r->height) >>1;

  XFillRectangle(r->d, r->w, r->prompt_bg, 0, 0, start_at, r->height);

  int text_len = strlen(text);
  if (text_len > prompt_width)
    text = text + (text_len - prompt_width);
  draw_string(r->ps1, r->ps1len, padding, texty, r, PROMPT);
  draw_string(text, MIN(text_len, prompt_width), padding + ps1xlen, texty, r, PROMPT);

  XFillRectangle(r->d, r->w, r->completion_bg, start_at, 0, r->width, r->height);

  while (cs != nil) {
    enum text_type tt = cs->selected ? COMPL_HIGH : COMPL;
    GC h = cs->selected ? r->completion_highlighted_bg : r->completion_bg;

    int len = strlen(cs->completion);
    int text_width = text_extents(cs->completion, len, r, nil, nil);

    XFillRectangle(r->d, r->w, h, start_at, 0, text_width + padding*2, r->height);

    draw_string(cs->completion, len, start_at + padding, texty, r, tt);

    start_at += text_width + padding * 2;

    if (start_at > r->width)
      break; // don't draw completion if the space isn't enough

    cs = cs->next;
  }

  XFlush(r->d);
}

// |-----------------------------------------------------------------|
// |  prompt                                                         |
// |-----------------------------------------------------------------|
// |  completion                                                     |
// |-----------------------------------------------------------------|
// |  completion                                                     |
// |-----------------------------------------------------------------|
// TODO: dunno why but in the call to Xutf8DrawString, by logic,
// should be padding and not padding*2, but the text doesn't seem to
// be vertically centered otherwise....
void draw_vertically(struct rendering *r, char *text, struct completions *cs) {
  int padding = 10; // TODO make this dynamic

  int height, width;
  text_extents("fjpgl", 5, r, &width, &height);
  int start_at = height + padding*2;

  XFillRectangle(r->d, r->w, r->completion_bg, 0, 0, r->width, r->height);
  XFillRectangle(r->d, r->w, r->prompt_bg, 0, 0, r->width, start_at);

  char *ps1_dup = strdupn(r->ps1);
  ps1_dup = ps1_dup == nil ? r->ps1 : ps1_dup;
  int ps1xlen = text_extents(ps1_dup, r->ps1len, r, nil, nil);
  free(ps1_dup);

  draw_string(r->ps1, r->ps1len, padding, padding*2, r, PROMPT);
  draw_string(text, strlen(text), padding + ps1xlen, padding*2, r, PROMPT);

  while (cs != nil) {
    enum text_type tt = cs->selected ? COMPL_HIGH : COMPL;
    GC h = cs->selected ? r->completion_highlighted_bg : r->completion_bg;

    int len = strlen(cs->completion);
    text_extents(cs->completion, len, r, &width, &height);
    XFillRectangle(r->d, r->w, h, 0, start_at, r->width, height + padding*2);
    draw_string(cs->completion, len, padding, start_at + padding*2, r, tt);

    start_at += height + padding *2;

    if (start_at > r->height)
      break; // don't draw completion if the space isn't enough

    cs = cs->next;
  }

  XFlush(r->d);
}

void draw(struct rendering *r, char *text, struct completions *cs) {
  if (r->horizontal_layout)
    draw_horizontally(r, text, cs);
  else
    draw_vertically(r, text, cs);
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

void get_wh(Display *d, Window *w, int *width, int *height) {
  XWindowAttributes win_attr;
  XGetWindowAttributes(d, *w, &win_attr);
  *height = win_attr.height;
  *width = win_attr.width;
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
  return 0;
}

void release_keyboard(Display *d) {
  XUngrabKeyboard(d, CurrentTime);
}

int parse_integer(const char *str, int default_value) {
  /* int len = strlen(str); */
  /* if (len > 0 && str[len-1] == '%') { */
  /*   char *cpy = strdup(str); */
  /*   check_allocation(cpy); */
  /*   cpy[len-1] = '\0'; */
  /*   int val = parse_integer(cpy, default_value, max); */
  /*   free(cpy); */
  /*   return val * max / 100; */
  /* } */

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

int parse_int_with_middle(const char *str, int default_value, int max, int self) {
  if (!strcmp(str, "middle")) {
    return (max - self)/2;
  }
  return parse_int_with_percentage(str, default_value, max);
}

int main() {
  char **lines = calloc(INITIAL_ITEMS, sizeof(char*));
  int nlines = readlines(&lines, INITIAL_ITEMS);

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

  char *ps1 = strdup("$ ");
  check_allocation(ps1);

  char *fontname = strdup(default_fontname);
  check_allocation(fontname);

  int textlen = 10;
  char *text = malloc(textlen * sizeof(char));
  check_allocation(text);

  struct completions *cs = filter(text, lines);
  bool nothing_selected = true;

  // start talking to xorg
  Display *d = XOpenDisplay(nil);
  if (d == nil) {
    fprintf(stderr, "Could not open display!\n");
    return EX_UNAVAILABLE;
  }

  // get display size
  // XXX: is getting the default root window dimension correct?
  XWindowAttributes xwa;
  XGetWindowAttributes(d, DefaultRootWindow(d), &xwa);
  int d_width = xwa.width;
  int d_height = xwa.height;

#ifdef USE_XINERAMA
  if (XineramaIsActive(d)) {
    // find the mice
    int number_of_screens = XScreenCount(d);
    bool result;
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
    compl_highlighted_fg, compl_highlighted_bg;

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
      char *v = strdup(value.addr);
      check_allocation(v);
      horizontal_layout = !strcmp(v, "horizontal");
      free(v);
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
      x = parse_int_with_middle(value.addr, x, d_width, width);
    else
      fprintf(stderr, "no x defined, using %d\n", width);

    if (XrmGetResource(xdb, "MyMenu.y", "*", datatype, &value) == true)
      y = parse_int_with_middle(value.addr, y, d_height, height);
    else
      fprintf(stderr, "no y defined, using %d\n", height);

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
  } else {
    XColor tmp;
    XAllocNamedColor(d, cmap, "white", &p_fg, &tmp);
    XAllocNamedColor(d, cmap, "black", &p_bg, &tmp);
    XAllocNamedColor(d, cmap, "white", &compl_fg, &tmp);
    XAllocNamedColor(d, cmap, "black", &compl_bg, &tmp);
    XAllocNamedColor(d, cmap, "black", &compl_highlighted_fg, &tmp);
    XAllocNamedColor(d, cmap, "white", &compl_highlighted_bg, &tmp);
  }

  // load the font
  /* XFontStruct *font = XLoadQueryFont(d, fontname); */
  /* if (font == nil) { */
  /*   fprintf(stderr, "Unable to load %s font\n", fontname); */
  /*   font = XLoadQueryFont(d, "fixed"); */
  /* } */
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

  Window w = XCreateWindow(d,                                   // display
                           DefaultRootWindow(d),                // parent
                           x + offset_x, y + offset_y,          // x y
                           width, height,                       // w h
                           0,                                   // border width
                           DefaultDepth(d, DefaultScreen(d)),   // depth
                           InputOutput,                         // class
                           DefaultVisual(d, DefaultScreen(d)),  // visual
                           CWOverrideRedirect,                  // value mask
                           &attr);

  set_win_atoms_hints(d, w, width, height);

  // we want some events
  XSelectInput(d, w, StructureNotifyMask | KeyPressMask | KeymapStateMask);

  // make the window appear on the screen
  XMapWindow(d, w);

  // wait for the MapNotify event (i.e. the event "window rendered")
  for (;;) {
    XEvent e;
    XNextEvent(d, &e);
    if (e.type == MapNotify)
      break;
  }

  // get the *real* width & height after the window was rendered
  get_wh(d, &w, &width, &height);

  // grab keyboard
  take_keyboard(d, w);

  // Create some graphics contexts
  XGCValues values;
  /* values.font = font->fid; */

  struct rendering r = {
    .d                          = d,
    .w                          = w,
#ifdef USE_XFT
    .font                       = font,
#else
    .font                       = &font,
#endif
    .prompt                     = XCreateGC(d, w, 0, &values),
    .prompt_bg                  = XCreateGC(d, w, 0, &values),
    .completion                 = XCreateGC(d, w, 0, &values),
    .completion_bg              = XCreateGC(d, w, 0, &values),
    .completion_highlighted     = XCreateGC(d, w, 0, &values),
    .completion_highlighted_bg  = XCreateGC(d, w, 0, &values),
    .width                      = width,
    .height                     = height,
    .horizontal_layout          = horizontal_layout,
    .ps1                        = ps1,
    .ps1len                     = strlen(ps1)
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

    case KeyPress: {
      XKeyPressedEvent *ev = (XKeyPressedEvent*)&e;

      if (ev->keycode == XKeysymToKeycode(d, XK_Tab)) {
        bool shift = (ev->state & ShiftMask);
        complete(cs, nothing_selected, shift, text, textlen, status);
        draw(&r, text, cs);
        break;
      }

      if (ev->keycode == XKeysymToKeycode(d, XK_BackSpace)) {
        nothing_selected = true;
        popc(text, textlen);
        update_completions(cs, text, lines);
        draw(&r, text, cs);
        break;
      }

      if (ev->keycode == XKeysymToKeycode(d, XK_Return)) {
        status = OK;
        break;
      }

      if (ev->keycode == XKeysymToKeycode(d, XK_Escape)) {
        status = ERR;
        break;
      }

      // try to read what the user pressed
      int symbol = 0;
      Status s = 0;
      Xutf8LookupString(xic, ev, (char*)&symbol, 4, 0, &s);

      if (s == XBufferOverflow) {
        // should not happen since there are no utf-8 characters
        // larger than 24bits, but is something to be aware of when
        // used to directly write to a string buffer
        fprintf(stderr, "Buffer overflow when trying to create keyboard symbol map.\n");
        break;
      }
      char *str = (char*)&symbol;

      if (ev->state & ControlMask) {
        // check for some key bindings
        if (!strcmp(str, "")) { // C-u
          nothing_selected = true;
          for (int i = 0; i < textlen; ++i)
            text[i] = 0;
          update_completions(cs, text, lines);
        }
        if (!strcmp(str, "")) { // C-h
          nothing_selected = true;
          popc(text, textlen);
          update_completions(cs, text, lines);
        }
        if (!strcmp(str, "")) { // C-w
          nothing_selected = true;

          // `textlen` is the length of the allocated string, not the
          // length of the ACTUAL string
          int p = strlen(text) - 1;
          if (p >= 0) { // delete the current char
            text[p] = 0;
            p--;
          }
          while (p >= 0 && isalnum(text[p])) {
            text[p] = 0;
            p--;
          }
          // erase also trailing white space
          while (p >= 0 && isspace(text[p])) {
            text[p] = 0;
            p--;
          }
          update_completions(cs, text, lines);
        }
        if (!strcmp(str, "\r")) { // C-m
          status = OK;
        }
        if (!strcmp(str, "")) {
          complete(cs, nothing_selected, true, text, textlen, status);
        }
        if (!strcmp(str, "")) {
          complete(cs, nothing_selected, false, text, textlen, status);
        }
        draw(&r, text, cs);
        break;
      }

      int str_len = strlen(str);
      for (int i = 0; i < str_len; ++i) {
        textlen = pushc(&text, textlen, str[i]);
        if (textlen == -1) {
          fprintf(stderr, "Memory allocation error\n");
          status = ERR;
          break;
        }
        nothing_selected = true;
        update_completions(cs, text, lines);
      }

    }
      draw(&r, text, cs);
      break;

    default:
      fprintf(stderr, "Unknown event %d\n", e.type);
    }
  }

  release_keyboard(d);

  if (status == OK)
    printf("%s\n", text);

  for (int i = 0; i < nlines; ++i) {
    free(lines[i]);
  }

#ifdef USE_XFT
    XftColorFree(d, DefaultVisual(d, 0), DefaultColormap(d, 0), &r.xft_prompt);
    XftColorFree(d, DefaultVisual(d, 0), DefaultColormap(d, 0), &r.xft_completion);
    XftColorFree(d, DefaultVisual(d, 0), DefaultColormap(d, 0), &r.xft_completion_highlighted);
#endif

  free(ps1);
  free(fontname);
  free(text);
  free(lines);
  compl_delete(cs);

  XDestroyWindow(d, w);
  XCloseDisplay(d);

  return status == OK ? 0 : 1;
}
