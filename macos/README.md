# Brhbp for macOS

A native app for the one printer this repo targets. Built on macOS 26 with
Swift 6.3.

    ./bundle.sh          # -> build/Brhbp.app
    open build/Brhbp.app

## Shape

    Sources/BrhbpBridge   flat C surface over the C++ engine (symlinks to ../cpp)
    Sources/BrhbpKit      PrintEngine actor, Device polling, shared types
    Sources/BrhbpApp      SwiftUI: PrintModel.swift, ContentView.swift
    Sources/brhbp-cli     the same engine, without the buttons

The engine sources are **symlinks into `../cpp`**, so there is exactly one copy
of the encoder in the repo and the conformance suite still governs it.

## Choices worth explaining

**A flat C bridge, not C++ interop.** Swift 6 imports C++ directly and for a
leaf type that would be fine. This is the app's ABI boundary, though, and a C
surface is trivially stable, trivially callable from an actor, and keeps
`std::string` out of a Swift module. The new language features earn their place
in the UI layer, not in the seam.

**One actor owns the socket and the encoder.** The C handle is not
thread-safe, so `PrintEngine` serialises everything -- except `requestCancel`,
which the C side deliberately makes safe from any thread. That is the single
concurrency exception and it is why cancelling from the UI needs no lock.

**Liquid Glass on exactly one surface.** The status bar floats over the page
grid, so content genuinely moves behind it: a `GlassEffectContainer` holding
the status pill and the device-cancel button, which refract together and
animate as one. The toolbar uses `.glassProminent` for Print and `.glass` for
Stop. Nothing else is glassed. Applying it everywhere is the anti-pattern.

**Previews come from the print renderer.** The same MuPDF renderer at 110 dpi
instead of 600, so what is on screen is what the encoder will see. They are
produced on a `PreviewRenderer` actor that owns its *own* document handle:
MuPDF's `fz_context` is not thread-safe, so previews are serialised among
themselves and cannot touch a job in flight. 21 pages of a paper render in
165 ms, about 7 ms each.

### Two bugs worth recording

**Previews rendered but never appeared.** Every thumbnail past the first sat on
a spinner forever. The renderer was fine -- all 21 pages produced correct
bitmaps in single-digit milliseconds. The fault was observation scope:

    ForEach(0..<model.pageCount, id: \.self) { i in
        PageThumb(index: i, image: model.previews[i])   // wrong

`ForEach`'s content closure runs *outside* the enclosing view's observation
scope, so reading `model.previews[i]` there registers no dependency and later
mutations invalidate nothing. Page 1 appeared only because its preview existed
before the grid was first built. Passing the model down and reading
`model.previews[index]` inside `PageThumb.body` puts the read back under
tracking. Nothing about this is visible in a failing build or a warning -- it
looks exactly like slow loading.

**Rendering blocked the window.** It also ran inline on the main actor, so a
screenful of thumbnails stalled the UI. It is now `async` on the actor, with an
in-flight set so a thumbnail that scrolls in and out is not rendered twice.

## Cancellation, which is the point

Two buttons, because there are two different things to stop.

**Stop** calls `requestCancel`. The writing task's next call fails, it calls
`abort`, and the encoder closes the raster escape with `1030M` and no form feed
-- so the page in flight is abandoned rather than ejected. Pages already handed
over have been committed and will still print; the status line says so rather
than pretending otherwise.

**Cancel on Printer** goes out of band over IPP on port 631 and is the only
thing that can reach a job the device has already accepted.

An app that sends to port 9100 without both of these has no way to stop a
running job, because the device offers no back-channel and PJL has no cancel
command. See `FORMAT.md` section 3.

## Status

Polled over SNMP every three seconds: reachability, `hrPrinterStatus`,
`hrPrinterDetectedErrorState` decoded into readable faults (out of paper, jam,
cover open, toner low) and the lifetime impression counter. SNMP works
concurrently with a print job, which a second connection to port 9100 does not.

## Verified on hardware

`brhbp-cli` exists so the print path can be driven and watched from a terminal.
It is not a reimplementation -- it links `BrhbpKit`, so it drives the identical
`PrintEngine` actor, bridge and band renderer the UI does.

    $ brhbp-cli --status --host 192.168.1.17
    Ready  faults=none  impressions=1916

    $ brhbp-cli ../test/sample.pdf --host 192.168.1.17 --paper A4 --dpi 600 --pages 1
    2:48:51.91  before: Ready, faults=none, impressions=1915
    2:48:51.91  connecting
    2:48:51.92  sending page 1/1
    2:48:52.17  sent 1 page(s)
    2:49:06.27  sheet out -> impressions=1916  (Ready)
    2:49:18.71  after: Ready, impressions=1916 (+1)

**250 ms of host work, then 14.1 s of engine.** That ratio is the whole design
argument in one line: the page was rendered, halftoned, encoded and on the wire
in a quarter of a second, and everything after that is paper moving.

The CLI refuses to start if the device reports a fault, and `--cancel-after N`
arms a watchdog that calls `requestCancel`. Both exist because sending to port
9100 without a way to stop is how a 250-page runaway happens.

## Not done

No sandbox entitlements or notarisation -- it is ad-hoc signed and local-only.
Printer discovery is manual: this model answers no mDNS at all, so there is
nothing to browse for and the address is typed in.
