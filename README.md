# MyMenu

> A drop-in replacement for dmenu, 'cause I was bored

![MyMenu works!](screen.png)

---

## What?

This is a drop-in replacement for `dmenu(1)`.

## Why?

This was the perfect excuse to learn how to make window with Xlib.

## How?

Check out the [manpage](mymenu.1) for further documentation.

---

## Dependencies

 - Xlib
 - Xinerama (optional)
   For multi-monitor support
 - pkg-config (optional)
   used in the makefile to generate `LIBS` and `CFLAGS` correctly

## Build

As simple as `make`. If you want to disable Xinerama support just
delete `-DUSE_XINERAMA` from the `CFLAGS` and `xinerama` from the
`pkg-config` call from the Makefile.

---

## TODO

 - Command line flags
 
   At the moment the X Resource Database is the only way to interact
   with the graphic appearance of MyMenu.

 - Optional TrueType support
 
 - Opacity support
 
 - Alternate layout
   
   An alternate layout (similar to rofi) would be nice too

## Scripts

I'm using this script to launch MyMenu

``` shell
#!/bin/sh

cat <<EOF | /bin/sh -c "$(mymenu "$@")"
ZZZ
sct 4500
lock
connect ethernet
connect home
connect phone
zzz
...
EOF
```

Of course you can as well use the `dmenu_path` and `dmenu_run` scripts
that (usually) comes with `dmenu`.
