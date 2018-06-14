MYMENU(1) - General Commands Manual

# NAME

**mymenu** - simple menu for XOrg

# DESCRIPTION

The
**mymenu**
utility a simple graphical menu for XOrg. It read the items from
**stdin**
and print the user selection to
**stdout**
on exit.

# OPTIONS

**-a**

> The first completion (if any) is always selected. This is like dmenu.

**-h**

> Print a small usage message to stderr.

**-v**

> Print version and exit.

# RESOURCES

The appearance of the menu is defined through the **X Resource**
Database.

MyMenu.font

> The font name to use. By default is set to "fixed" if compiled without
> Xft(3) support, "monospace" otherwise. Without Xft(3) only bitmap font
> are supported.

MyMenu.layout

> The layout of the menu. The possible values are "horizontal" and
> "vertical", with the default being "horizontal". Every other value
> than "horizontal" is treated like "vertical", but this is kinda an
> implementation detail and not something to be relied on, since in the
> future other layout could be added as well.

Mymenu.prompt

> A string that is rendered before the user input. Default to "$ ".

MyMenu.width

> The width of the menu. If a numeric value is given (e.g. 400) is
> interpreted as pixel, if it ends with a percentage symbol \`%'
> (e.g. 40%) the relative percentage will be computed (relative to the
> monitor width).

MyMenu.height

> The height of the menu. Like MyMenu.width if a numeric value is given
> is interpreted as pixel, if it ends with a percentage symbol \`%' the
> relative percentage will be computed (relative to the monitor height).

MyMenu.x

> The X coordinate of the topmost left corner of the window. Much like
> MyMenu.height and MyMenu.width both a pixel dimension and percentage
> could be supplied. In addition to it, the special value "middle" could
> be used: in that case the window will be centered on the x axes.

MyMenu.y

> The Y coordinate of the topmost left corner of the window. Like the X
> coordinate a pixel dimension, percentage dimension or the special
> value "middle" could be supplied.

MyMenu.padding

> Change the padding. In the horizontal layout the padding is the space
> between the rectangle of the completion and the text as well as the
> space between the prompt and the first completion. In the horizontal
> layout the padding is the horizontal spacing between the window edge
> and the text as well as the space up and down the text within the
> completion. The default value is 10.

MyMenu.prompt.background

> The background of the prompt.

MyMenu.prompt.foreground

> The text color (foreground) of the prompt.

MyMenu.completion.background

> The background of the completions.

MyMenu.completion.foreground

> The text color of the completions.

MyMenu.completion\_highlighted.background

> The background of the selected completion.

MyMenu.completion\_highlighted.foreground

> The foreground of the selected completion.

# KEYS

Esc

> Close the menu without selecting any entry

Enter

> Close the menu and print to stdout what the user typed

C-m

> The same as Enter

Tab

> Expand the prompt to the next possible completion

Shift Tab

> Expand the prompt to the previous possible completion

C-n

> The same as Tab

C-p

> The same as Shift-Tab

Backspace

> Delete the last character

C-h

> The same as Backspace

C-w

> Delete the last word

C-u

> Delete the whole line

# BUGS

*	If, instead of a numeric value, a not-valid number that terminates
	with the % sign is supplied, then the default value for that field
	will be treated as a percentage. Since this is a misuse of the
	resources this behavior isn't strictly considered a bug.

*	C-w (delete last word) does not work well with multi-byte string. The
	whole UTF-8 support is still kinda na&#239;ve and should be improved.

# EXIT STATUS

0 when the user select an entry, 1 when the user press Esc, EX\_USAGE
if used with wrong flags and EX\_UNAVAILABLE if the connection to X
fails.

# SEE ALSO

dmenu(1)
sysexits(3)

# AUTHORS

Omar Polo &lt;omar.polo@europecom.net&gt;

OpenBSD 6.3 - June 14, 2018
