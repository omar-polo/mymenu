#include <X11/Xcms.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>

#include <X11/extensions/Xinerama.h>

int
main(void)
{
	XOpenDisplay(NULL);
	return (0);
}
