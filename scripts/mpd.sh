#!/bin/sh

fmt="%position% %artist - %title%"
ps1="Song: "

if song=$(mpc playlist -f "$fmt" | mymenu -A -p "$ps1" -d " "); then
	mpc play $(echo $song | sed "s/ .*$//")
fi
