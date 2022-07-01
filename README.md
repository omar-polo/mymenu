# MyMenu

> A menu for Xorg, 'cause I was bored

![MyMenu works!](screen.png)

![MyMenu alternate layout](screen-alt.png)

---

## What?

This is a simple menu for Xorg, like `dmenu(1)`.

## Why?

This was the perfect excuse to learn how to use Xlib.

## How?

Check out the [manpage](mymenu.1.md) for further documentation. Check
out also the [template](Xexample) for the resources.

---

## Features

- two layout: `horizontal` (a là dmenu) and `vertical` (a là rofi);
- highly customizable (width, height, position on the screen, colors, borders, ...);
- transparency support
- support for both Xft and bitmap font

## Dependencies

 - Xlib
 - Xinerama for multi-monitor support
 - Xft for TrueType font support
 - pkg-config *(optional)* to aid the autoconfiguration
 - mandoc *(optional)* to generate the
   [markdown version of the manpage](mymenu.1.md)

## Build

The usual spell:

	$ ./configure
	$ make

## FAQ

 - Does not run / Hangs

   At the startup mymenu will read `stdin` for a list of item, only
   then it'll display a window. Are you sure that you're passing
   something on standard input?

 - Will feature $X be added?

   No. Or maybe yes. In fact, it depends. Open an issue and let's
   discuss. If it's something that's trivial to achieve in combo with
   other tool maybe is not the case to add it here.

 - Is feature $Y present? What $Z do? How to achieve $W?

   Everything is documented in the [man page](mymenu.1.md). To read
   it, simply execute `man -l mymenu.1` or `mandoc mymenu.1 | less`
   (depending on your system the `-l` option may not be present).

---

## Scripts

I'm using this script to launch MyMenu with custom item

``` shell
#!/bin/sh

cat <<EOF | /bin/sh -c "$(mymenu "$@")"
sct 4500
lock
connect ethernet
connect home
connect phone
ZZZ
zzz
...
EOF
```

You can generate a list of executables from `$PATH` like this:

``` shell
#!/bin/sh

path=`echo $PATH | sed 's/:/ /g'`

{
	for p in $path; do
		for f in "$p"/*; do
			[ -x "$f" ] && echo "${f##*/}"
		done
	done
} | sort -fu | /bin/sh -c "$(mymenu "$@")"
```

You can, for example, select a song to play from the current queue of [amused][amused]

```shell
if song=$(amused show | mymenu -p "Song: " -A); then
	amused jump "$song"
fi
```

The same, but with mpd:

```shell
fmt="%position% %artist% - %title%"
if song=$(mpc playlist -f "$fmt" | mymenu -p "Song: " -A -d " "); then
    mpc play $(echo $song | sed "s/ .*$//")
fi
```

Of course you can as well use the `dmenu_path` and `dmenu_run` scripts
that (usually) comes with `dmenu`.

[amused]: https://projects.omarpolo.com/amused.html
