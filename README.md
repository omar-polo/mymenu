# MyMenu

> A replacement for dmenu, 'cause I was bored

![MyMenu works!](screen.png)

![MyMenu alternate layout](screen-alt.png)

---

## What?

This is a replacement for `dmenu(1)`.

## Why?

This was the perfect excuse to learn how to use Xlib.

## How?

Check out the [manpage](mymenu.1.md) for further documentation. Check
out also the [template](Xexample) for the resources.

---

## Dependencies

 - Xlib
 - Xinerama (optional) for multi-monitor support
 - Xft (optional) for TrueType font support
 - pkg-config (optional) used in the makefile to generate `LIBS` and
   `CFLAGS` correctly

## Build

As simple as `make` (or `make gnu` if you're using GNU libc). Keep in
mind that, by default, both Xft and Xinerama are enabled. So, you may
want to run:

  - `make no_xft` to build without xft support;
  - `make no_xinerama` to build without xinerama support;
  - `make no_xft_xinerama` to build without xinerama *and* no xft support.

#### ignore case completion / don't have `strcasestr(3)`

If you want to build without the ignore case completion or on your
platform `strcasestr(3)` isn't available, you have to update the
`Makefile` and remove `-DUSE_STRCASESTR`. A simple
``` shell
sed -i.orig 's/-DUSE_STRCASESTR//g' Makefile
```
should be enough.


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

## TODO

 - Improve UTF8 support

   The whole UTF8 support is still kinda naïve and should definitely
   be improved.

 - Opacity support

 - Command line flags

   At the moment the X Resource Database is the only way to interact
   with the graphic appearance of MyMenu.

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

You can generate a menu from the `.desktop` file with something like
this:

``` shell
#!/bin/sh

getname() {
    cat $1 | grep '^Name=' | sed 's/^.*=//'
}

getexec() {
    cat $1 | grep '^Exec=' | sed 's/^.*=//'
}

desktop_files=`ls /usr/local/share/applications/*.desktop`

{
    for i in $desktop_files; do
        getname $i
    done
} | mymenu "$@" | {
    read prgname
    for i in $desktop_files; do
        name=`getname $i`
        if [ "x$prgname" = "x$name" ]; then
            exec `getexec $i`
        fi
    done
}
```

or generate a list of executables from `$PATH` like this:

``` shell
#!/bin/sh

path=`echo $PATH | sed 's/:/ /g'`

{
    for i in $path; do
        ls -F $i | grep '.*\*$' | sed 's/\*//'
    done
} | sort -f | /bin/sh -c "$(mymenu "$@")"
```

Of course you can as well use the `dmenu_path` and `dmenu_run` scripts
that (usually) comes with `dmenu`.
