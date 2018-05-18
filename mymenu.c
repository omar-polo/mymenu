/* mymenu -- simple dmenu alternative */

/* Copyright (C) 2018  Omar Polo <omar.polo@europecom.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

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

#ifdef USE_XINERAMA
# include <X11/extensions/Xinerama.h>
#endif

#define nil NULL

#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define OVERLAP(a,b,c,d) (((a)==(c) && (b)==(d)) || MIN((a)+(b), (c)+(d)) - MAX((a), (c)) > 0)
#define INTERSECT(x,y,w,h,x1,y1,w1,h1) (OVERLAP((x),(w),(x1),(w1)) && OVERLAP((y),(h),(y1),(h1)))

#define update_completions(cs, text, lines) {   \
    compl_delete(cs);                           \
    cs = filter(text, lines);                   \
  }

// TODO: dynamic?
#define MAX_ITEMS 256

#define TODO(s) {                               \
    fprintf(stderr, "TODO! " s "\n");           \
  }

#define cannot_allocate_memory {                        \
    fprintf(stderr, "Could not allocate memory\n");     \
    exit(EX_UNAVAILABLE);                               \
  }

#define check_allocation(a) {     \
    if (a == nil)                 \
      cannot_allocate_memory;     \
  }

enum state {LOOPING, OK, ERR};

struct rendering {
  Display *d;
  Window w;
  GC prompt;
  GC prompt_bg;
  GC completion;
  GC completion_bg;
  GC completion_highlighted;
  GC completion_highlighted_bg;
  int width;
  int height;
  XFontStruct *font;
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

struct completions *filter(char *text, char **lines) {
  int i = 0;
  struct completions *root = compl_new();
  struct completions *c = root;

  for (;;) {
    char *l = lines[i];
    if (l == nil)
      break;

    if (strstr(l, text) != nil) {
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

void popc(char *p, int maxlen) {
  int len = strnlen(p, maxlen);
  p[len-1] = '\0';
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

int readlines (char **lines) {
  bool finished = false;
  int n = 0;
  while (n < MAX_ITEMS) {
    lines[n] = readline(&finished);

    if (strlen(lines[n]) == 0 || lines[n][0] == '\n')
      --n; // forget about this line

    if (finished)
      break;

    ++n;
  }
  /* for (n = 0; n < MAX_ITEMS -1; ++n) { */
  /*   lines[n] = readline(&finished); */
  /*   if (finished) */
  /*     break; */
  /* } */
  n++;
  lines[n] = nil;
  return n;
}

// |------------------|----------------------------------------------|
// | 20 char text     | completion | completion | completion | compl |
// |------------------|----------------------------------------------|
void draw(struct rendering *r, char *text, struct completions *cs) {
  int texty = (r->height + r->font->ascent) >> 1;

  // TODO: make these dynamic?
  int prompt_width = 20; // char
  int padding = 10;
  int start_at = XTextWidth(r->font, " ", 1) * prompt_width + padding;

  XFillRectangle(r->d, r->w, r->prompt_bg, 0, 0, start_at, r->height);

  int text_len = strlen(text);
  if (text_len > prompt_width)
    text = text + (text_len - prompt_width);
  XDrawString(r->d, r->w, r->prompt, padding, texty, text, MIN(text_len, prompt_width));

  XFillRectangle(r->d, r->w, r->completion_bg, start_at, 0, r->width, r->height);

  while (cs != nil) {
    GC g = cs->selected ? r->completion_highlighted    : r->completion;
    GC h = cs->selected ? r->completion_highlighted_bg : r->completion_bg;

    int len = strlen(cs->completion);
    int text_width = XTextWidth(r->font, cs->completion, len);

    XFillRectangle(r->d, r->w, h, start_at, 0, text_width + padding*2, r->height);
    XDrawString(r->d, r->w, g, start_at + padding, texty, cs->completion, len);

    start_at += text_width + padding * 2;

    cs = cs->next;
  }

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
  class_hint->res_name = "mymenu";
  class_hint->res_class = "mymenu";
  XSetClassHint(d, w, class_hint);
  XFree(class_hint);

  XSizeHints *size_hint = XAllocSizeHints();
  if (size_hint == nil) {
    fprintf(stderr, "Could not allocate memory for size hint\n");
    exit(EX_UNAVAILABLE);
  }
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

int parse_integer(const char *str, int default_value, int max) {
  int len = strlen(str);
  if (len > 0 && str[len-1] == '%') {
    char *cpy = strdup(str);
    check_allocation(cpy);
    cpy[len-1] = '\0';
    int val = parse_integer(cpy, default_value, max);
    free(cpy);
    return val * max / 100;
  }

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

int parse_int_with_middle(const char *str, int default_value, int max, int self) {
  if (!strcmp(str, "middle")) {
    return (max - self)/2;
  }
  return parse_integer(str, default_value, max);
}

int main() {
  char *lines[MAX_ITEMS] = {0};
  readlines(lines);

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

  char *fontname = strdup("fixed");
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
  // TODO: this bit still needs to be improved
  if (XineramaIsActive(d)) {
    int monitors;
    XineramaScreenInfo *info = XineramaQueryScreens(d, &monitors);
    if (info)
      for (int i = 0; i < monitors; ++i) {
        if (INTERSECT(x, y, 1, 1, info[i].x_org, info[i].y_org, info[i].width, info[i].height)) {
          offset_x = info[x].x_org;
          offset_y = info[y].y_org;
          d_width = info[i].width;
          d_height = info[i].height;
        }
      }
    XFree(info);
  }
#endif

  /* fprintf(stderr, "offset_x:\t%d\n" */
  /*                 "offset_y:\t%d\n" */
  /*                 "d_width:\t%d\n" */
  /*                 "d_height:\t%d\n" */
  /*         , offset_x, offset_y, d_width, d_height); */

  Colormap cmap = DefaultColormap(d, DefaultScreen(d));
  XColor p_fg, p_bg,
    compl_fg, compl_bg,
    compl_highlighted_fg, compl_highlighted_bg;

  // read resource
  XrmInitialize();
  char *xrm = XResourceManagerString(d);
  if (xrm != nil) {
    XrmDatabase xdb = XrmGetStringDatabase(xrm);
    XrmValue value;
    char *datatype[20];

    if (XrmGetResource(xdb, "MyMenu.font", "*", datatype, &value) == true) {
      fontname = strdup(value.addr);
      check_allocation(fontname);
    }
    else
      fprintf(stderr, "no font defined, using %s\n", fontname);

    if (XrmGetResource(xdb, "MyMenu.width", "*", datatype, &value) == true)
      width = parse_integer(value.addr, width, d_width);
    else
      fprintf(stderr, "no width defined, using %d\n", width);

    if (XrmGetResource(xdb, "MyMenu.height", "*", datatype, &value) == true)
      height = parse_integer(value.addr, height, d_height);
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
  XFontStruct *font = XLoadQueryFont(d, fontname);
  if (font == nil) {
    fprintf(stderr, "Unable to load %s font\n", fontname);
    font = XLoadQueryFont(d, "fixed");
  }

  // create the window
  XSetWindowAttributes attr;

  Window w = XCreateWindow(d,                                   // display
                           DefaultRootWindow(d),                // parent
                           x + offset_x, y + offset_y,          // x y
                           width, height,                       // w h
                           0,                                   // border width
                           DefaultDepth(d, DefaultScreen(d)),   // depth
                           InputOutput,                         // class
                           DefaultVisual(d, DefaultScreen(d)),  // visual
                           0,                                   // value mask
                           &attr);

  set_win_atoms_hints(d, w, width, height);

  // we want some events
  XSelectInput(d, w, StructureNotifyMask | KeyPressMask | KeyReleaseMask | KeymapStateMask);

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
  values.font = font->fid;

  struct rendering r = {
    .d                          = d,
    .w                          = w,
    .prompt                     = XCreateGC(d, w, GCFont, &values),
    .prompt_bg                  = XCreateGC(d, w, GCFont, &values),
    .completion                 = XCreateGC(d, w, GCFont, &values),
    .completion_bg              = XCreateGC(d, w, GCFont, &values),
    .completion_highlighted     = XCreateGC(d, w, GCFont, &values),
    .completion_highlighted_bg  = XCreateGC(d, w, GCFont, &values),
    .width                      = width,
    .height                     = height,
    .font                       = font
  };

  // load the colors in our GCs
  XSetForeground(d, r.prompt, p_fg.pixel);
  XSetForeground(d, r.prompt_bg, p_bg.pixel);
  XSetForeground(d, r.completion, compl_fg.pixel);
  XSetForeground(d, r.completion_bg, compl_bg.pixel);
  XSetForeground(d, r.completion_highlighted, compl_highlighted_fg.pixel);
  XSetForeground(d, r.completion_highlighted_bg, compl_highlighted_bg.pixel);

  // draw the window for the first time
  draw(&r, text, cs);

  // main loop
  while (status == LOOPING) {
    XEvent e;
    XNextEvent(d, &e);

    switch (e.type) {
    case KeymapNotify:
      XRefreshKeyboardMapping(&e.xmapping);
      break;

    case KeyRelease: break; // ignore this

    case KeyPress:
      switch (e.xkey.keycode) {
      case 0x024: // enter
        status = OK;
        break;

      case 0x09: // esc
        status = ERR;
        break;

      case 0x017: { // tab
        // TODO: re-organize this mess of code.
        // FIXME: shift detection does not work!
        if ((e.xkey.state | ShiftMask) == 0) { // shift?
          fprintf(stderr, "SHIFT\n");
          if (nothing_selected || (cs != nil && cs->selected)) { // select the last one
            if (cs != nil && cs->selected)
              cs->selected = false;

            struct completions *cc = cs;
            while (cc != nil) {
              if (cc->next == nil) {
                cc->selected = true;
                free(text);
                text = strdup(cc->completion);
                if (text == nil) {
                  fprintf(stderr, "Memory allocation error!");
                  status = ERR;
                  break;
                }
                textlen = strlen(text);
                break;
              }
              cc = cc->next;
            }
            nothing_selected = false;
          } else {
            puts("select the the previous one");
            struct completions *cc = cs;
            while (cc != nil) {
              if (cc->next != nil && cc->next->selected) {
                cc->selected = true;
                cc->next->selected = false;
                free(text);
                text = strdup(cc->next->completion);
                if (text == nil) {
                  fprintf(stderr, "Memory allocation error!");
                  status = ERR;
                  break;
                }
                textlen = strlen(text);
                break;
              }
              cc = cc ->next;
            }
          }
        } else {
          fprintf(stderr, "no SHIFT\n");
          if (nothing_selected) {
            if (cs != nil) {
              cs->selected = true;
              nothing_selected = false;
              free(text);
              text = strdup(cs->completion);
              if (text == nil) {
                fprintf(stderr, "Memory allocation error!");
                status = ERR;
                break;
              }
              textlen = strlen(text);
            }
          } else {
            struct completions *cc = cs;
            while (cc != nil) {
              if (cc->selected) {
                struct completions *n = cc->next != nil ? cc->next : cs;

                cc->selected = false;
                n->selected = true;

                free(text);
                text = strdup(n->completion);
                if (text == nil) {
                  fprintf(stderr, "Memory allocation error!");
                  status = ERR;
                  break;
                }
                textlen = strlen(text);
                break;
              }
              cc = cc->next;
            }
          }
        }

        draw(&r, text, cs);
        break;
      }

      case 0x16: // backspace
        nothing_selected = true;
        popc(text, textlen);

        update_completions(cs, text, lines);

        draw(&r, text, cs);
        break;

      default: {
        char string[255] = {0};
        KeySym keysym;
        int len = XLookupString(&e.xkey, string, 25, &keysym, nil);
        if (len > 0 && (isalnum(string[0]) || string[0] == ' ' || string[0] == '-')) {
          textlen = pushc(&text, textlen, string[0]);

          if (textlen == -1) { // uh oh
            fprintf(stderr, "Memory allocation error!");
            status = ERR;
            break;
          }

          nothing_selected = true;
          update_completions(cs, text, lines);
        }
        break;
      }
      } // keypress

      draw(&r, text, cs);
      break;

    default:
      fprintf(stderr, "unknown event %d\n", e.type);
    }
  }

  release_keyboard(d);

  if (status == OK)
    printf("%s\n", text);

  free(fontname);
  free(text);
  compl_delete(cs);

  /* XDestroyWindow(d, w); */
  XCloseDisplay(d);

  return status == OK ? 0 : 1;
}
