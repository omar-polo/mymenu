MYMENU(1) - General Commands Manual

# NAME

**mymenu** - simple menu for XOrg

# SYNOPSIS

**mymenu**
\[**-Aahmv**]
\[**-B**&nbsp;*colors*]
\[**-b**&nbsp;*size*]
\[**-C**&nbsp;*color*]
\[**-c**&nbsp;*color*]
\[**-d**&nbsp;*separator*]
\[**-e**&nbsp;*window*]
\[**-f**&nbsp;*font*]
\[**-G**&nbsp;*color*]
\[**-g**&nbsp;*size*]
\[**-H**&nbsp;*height*]
\[**-I**&nbsp;*color*]
\[**-i**&nbsp;*size*]
\[**-J**&nbsp;*color*]
\[**-j**&nbsp;*size*]
\[**-l**&nbsp;*layout*]
\[**-P**&nbsp;*padding*]
\[**-p**&nbsp;*prompt*]
\[**-S**&nbsp;*color*]
\[**-s**&nbsp;*color*]
\[**-T**&nbsp;*color*]
\[**-t**&nbsp;*color*]
\[**-W**&nbsp;*width*]
\[**-x**&nbsp;*coord*]
\[**-y**&nbsp;*coord*]

# DESCRIPTION

The
**mymenu**
utility a simple graphical menu for XOrg. It read the items from
**stdin**
and print the user selection to
**stdout**
on exit.

The following options are available and take the maximum precedence
over the (respective) ones defined in the
**X Resource Database**

**-A**

> The user must chose one of the option (or none) and is not able to
> arbitrary enter text

**-a**

> The first completion (if any) is always selected. This is like dmenu.

**-B** *colors*

> Override the borders color. Parsed as MyMenu.border.color.

**-b** *borders*

> Override the borders size. Parsed as MyMenu.border.size.

**-C** *color*

> Override the completion background color. See
> MyMenu.completion.background.

**-c** *color*

> Override the completion foreground color. See
> MyMenu.completion.foreground.

**-d** *separator*

> Define a string to be used as a separator (mnemonic: delimiter). Only
> the text after the separator will be rendered, but the original string
> will be returned. Useful to embed custom data on every entry. See the
> mpd example for hints on how this can be useful.

**-e** *windowid*

> Embed into the given window id.

**-f** *font*

> Override the font. See MyMenu.font.

**-G** *color*

> Override the border colors of the prompt. See
> MyMenu.prompt.border.color.

**-g** *size*

> Override the border size of the prompt. See
> MyMenu.prompt.border.size.

**-H** *val*

> Override the height. Parsed as MyMenu.height.

**-h**

> Print a small usage message to stderr.

**-I** *color*

> Override the border colors of the completion. See
> MyMenu.completion.border.color.

**-i** *size*

> Override the border size of the completion. See
> MyMenu.completion.border.size.

**-J** *color*

> Override the border colors of the completion. See
> MyMenu.completion\_highlighted.border.color.

**-j** *size*

> Override the border size of the completion. See
> MyMenu.completion\_highlighted.border.size.

**-l** *layout*

> Override the layout. Parsed as MyMenu.layout.

**-m**

> The user can select multiple entry via C-m. Please consult
> *KEYS*
> for more info.

**-P** *padding*

> Override the padding. See the MyMenu.prompt.padding resource.

**-p** *prompt*

> Override the prompt

**-S** *color*

> Override the highlighted completion background color. See
> MyMenu.completion\_highlighted.background.

**-s** *color*

> Override the highlighted completion foreground color. See
> MyMenu.completion\_highlighted.foreground.

**-T** *color*

> Override the prompt background color. See MyMenu.prompt.background.

**-t** *color*

> Override the prompt foreground color. See MyMenu.prompt.foreground.

**-v**

> Print version and exit.

**-W** *val*

> Override the width. Parsed as MyMenu.width.

**-x** *val*

> Override the positioning on the X axis, parsed as the resource MyMenu.x

**-y** *val*

> Override the positioning on the Y axis, parsed as the resource MyMenu.y

# RESOURCES

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

MyMenu.prompt

> A string that is rendered before the user input. Default to "$ ".

MyMenu.prompt.border.size

> Parsed like MyMenu.border.size, but affects only the prompt. Default
> to 0.

MyMenu.prompt.border.color

> Parsed like MyMenu.border.color, but affects only the prompt. Default
> to #000 (black).

MyMenu.prompt.padding

> Parsed like MyMenu.border.size. The padding is the space between the
> end of the borderd and the start of the text, in any direction (top,
> bottom, left, right). Default to 10.

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
> could be supplied. In addition to it, some special value can be used.

> start

> > Alias for 0;

> middle or center

> > Compute the correct value to make sure that mymenu will be
> > horizontally centered;

> end

> > Compute the correct value to make sure that mymenu will be right
> > aligned.

MyMenu.y

> The Y coordinate of the topmost left corner of the window. Like the X
> coordinate a pixel dimension, percentage dimension or the special
> value "start", "middle"/"center", "end" could be supplied.

MyMenu.border.size

> A list of number separated by spaces to specify the border of the
> window. The field is parsed like some CSS properties (i.e. padding),
> that is: if only one value is provided then it'll be used for all
> borders; if two value are given than the first will be used for the
> top and bottom border and the former for the left and right border;
> with three value the first is used for the top border, the second for
> the left and right border and the third for the bottom border. If four
> value are given, they'll be applied to the respective border
> clockwise. Other values will be ignored. The default value is 0.

> This particular option accepts a percentage (e.g. 10%) relative to the
> screen width.

MyMenu.border.color

> A list of colors for the borders. This field is parsed like the
> MyMenu.border.size. The default value is black.

MyMenu.prompt.background

> The background of the prompt.

MyMenu.prompt.foreground

> The text color (foreground) of the prompt.

MyMenu.completion.background

> The background of the completions.

MyMenu.completion.foreground

> The text color of the completions.

MyMenu.completion.border.size

> Parsed like MyMenu.border.size, but affects only the
> completion. Default to 0.

MyMenu.completion.border.color

> Parsed like MyMenu.border.color, but affects only the
> completion. Default to #000 (black).

MyMenu.completion.padding

> Parsed like MyMenu.prompt.padding. Default to 10.

MyMenu.completion\_highlighted.background

> The background of the selected completion.

MyMenu.completion\_highlighted.foreground

> The foreground of the selected completion.

MyMenu.completion\_highlighted.border.size

> Parsed like MyMenu.border.size, but affects only the completion
> highlighted. Default to 0.

MyMenu.completion\_highlighted.border.color

> Parsed like MyMenu.border.color, but affects only the completion
> highlighted. Default to #000 (black).

MyMenu.completion\_highlighted.padding

> Parsed like MyMenu.prompt.padding. Default to 10.

# COLORS

MyMenu accept colors only in the form of:

*	#rgb

*	#rrggbb

*	#aarrggbb

The opacity is assumed 0xff (no transparency) if not provided.

# KEYS

This is the list of keybinding recognized by
**mymenu**.
In the following examples, C-c means Control-c.

Esc

> Close the menu without selecting any entry

C-c

> The same as Esc

Enter

> Close the menu and print to stdout what the user typed

C-m

> Confirm but keep looping (if enabled), otherwise complete only

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

C-i

> Toggle the \`\`first selected'' style. Sometimes, especially with the -a
> option, could be handy to disable that behaviour. Let's say that
> you've typed \`\`fire'' and the first completion is \`\`firefox'' but you
> really want to choose \`\`fire''. While you can type some spaces, this
> keybinding is a more elegant way to change, at runtime, the behaviour
> of the first completion.

# EXIT STATUS

0 when the user select an entry, 1 when the user press Esc, EX\_USAGE
if used with wrong flags and EX\_UNAVAILABLE if the connection to X
fails.

# EXAMPLES

*	Create a simple menu with a couple of entry

		cat <<EOF | $SHELL -c "$(mymenu -p "Exec: ")"
		firefox
		zzz
		xcalc -stipple
		xlock
		gimp
		EOF

*	Select and play a song from the current mpd playlist

		fmt="%position% %artist% - %title%"
		if song=$(mpc playlist -f "$fmt" | mymenu -p "Song: " -A -d " "); then
		    mpc play $(echo $song | sed "s/ .*$//")
		fi

# SEE ALSO

dmenu(1)
sysexits(3)

# AUTHORS

Omar Polo &lt;omar.polo@europecom.net&gt;

# CAVEATS

*	If, instead of a numeric value, a not-valid number that terminates
	with the % sign is supplied, then the default value for that field
	will be treated as a percentage. Since this is a misuse of the
	resources this behavior isn't strictly considered a bug.

*	Keep in mind that sometimes the order of the options matter. First are
	parsed (if any) the xrdb options, then the command line flags
	**in the provided order!**
	That meas that if you're providing first the x coordinate, let's say
	"middle", and
	**after that**
	you are overriding the width, the window
	**will not be**
	centered.

	As a general rule of thumb, if you're overriding the width and/or the
	height of the window, remember to override the x and y coordinates as
	well.

Void Linux - October 18, 2019
