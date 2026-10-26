# WetChorus WebCLAP

[WetChorus](https://github.com/yonie/WetChorus) by Ronald Klarenbeek (Yonie), MIT, as a WebCLAP audio effect. The DSP
and the panel art are taken unchanged from commit `b9977a76171e8ab996b61a2def1c0acd6a02f5ee`, only the VST3 glue is
replaced by a CLAP plugin and the VSTGUI editor by a web page.

A tech demo of openDAW's device panel for audio effects, so it is neither on the site nor in openDAW's catalogue.
`scripts/build-wet-chorus.sh` builds `build/wet-chorus/wet-chorus.wclap.tar.gz`, for openDAW's Import WebCLAP.

## Layout

```
build.sh        builds module.wasm and copies the page, its images and LICENSE into $OUT (CLAP headers in $CLAP_INCLUDE)
LICENSE         WetChorus's MIT license, it covers this port too
upstream/       wetcore.h, bbdcore.h, chorusengine.h/.cpp and meterlevel.h from WetChorus/source, unchanged
plugin/         wetchorus.cpp, the CLAP plugin around the engine
page/           index.html and resource/ (backplate, knob filmstrips, the DEPTH button, lamp and glow, unchanged)
```

## The plugin

- Parameters: Speed 0 to 10 and Mode 0 to 180 degrees, both snapped to the 65 knob positions, Depth 0 or 1. The
  texts are the ones the VST3 controller prints.
- The saved state is upstream's: version 1, the step count, then the speed, depth and mode positions (int32, little
  endian), so a later step count still reads it.
- Stereo in and out, the engine sums the input to mono as the circuit does.

## The page

One page for both places. In the window every CLAP host shows it at 768 x 512, resizable from 50 % to 125 % like the
original's UI Zoom. openDAW also shows it in its device panel (feature `opendaw:device-panel`, extension
`opendaw.device-panel/1`): the whole panel scaled to the panel height, 264 x 176.

All of it is drawn on one canvas at the screen's pixel density. The art is made for 1x at 768 x 512, so the device panel
(a third of that) is sharp at 2x and 3x, the window is sharp at 2x when zoomed to 50 %.

Every control reports `clap.param-hovered`, so a host can offer its parameter menu on right-click and value entry on
double-click. Drags ignore ctrl+click, macOS's context click.
