bmdkey
======

What?
-----

This is a modified version of the original tool, described simple and fairly crude tool for turning a Blackmagic Design Speed
Editor into somethinf more useful while not using DaVinci Resolve.

The modified settings allow the user to use the tool in programs that use the space bar for media playback, in and out points with i and o keys
and frame by frame advance with the arrow keys.

Space bar is mapped to stop/play
in key sends out i
out key sends out o
jog wheel sends out left or right arrow keys

A crude GUI in python has been added to start and stop the program.
Note that the path for the bmdkey executable has to be edited in the bdm_gui.py file

To further customize the tool just edit the main.cc file and recompile

The original description is as follows:

This only works if DaVinci Resolve isn't open.

It uses [smunaut's algorithm for unlocking the
device](https://github.com/smunaut/blackmagic-misc). Without this, the device is
a paperweight unless you have Resolve running.

[![Watch me write it!](https://img.youtube.com/vi/UoIlwze5xp4/0.jpg)](https://www.youtube.com/watch?v=UoIlwze5xp4)

Why?
----

I found having a nice piece of hardware which was locked down to only work with
one particular application offensive.

Where?
------

It's [open source on GitHub!](https://github.com/davidgiven/bmdkey)

How?
----

Build it with `make`. You'll need `libfakekey`, `hidapi-libusb` and `fmt`. Then
just run the `bmdkey` application with the keyboard connected up.

There are no configuration options. Also, if you unplug the Speed Editor, it'll
crash.


You _should_ be able to use this on other BMD hardware, but I haven't tried. If
anyone wants to send me some incredibly expensive keyboards for free, I'll quite
happily do a port!

Who?
----

You may contact me at dg@cowlark.com, or visit my website at
http://www.cowlark.com.  There may or may not be anything interesting there.


License
-------

Everything here is © 2024 David Given, and is licensed under the two-clause BSD open source
license. Please see [LICENSE](LICENSE) for the full text. The tl;dr is: you can
do what you like with it provided you don't claim you wrote it.

