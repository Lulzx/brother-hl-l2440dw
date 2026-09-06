# Brhbp for macOS

A native app for the one printer this repo targets. Built on macOS 26 with
Swift 6.3.

    ./bundle.sh          # -> build/Brhbp.app
    open build/Brhbp.app

## Shape

    Sources/BrhbpBridge   flat C surface over the C++ engine (symlinks to ../cpp)
    Sources/BrhbpApp      SwiftUI app: Engine.swift, PrintModel.swift, ContentView.swift

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

**Previews come from the print renderer.** The same MuPDF band renderer, run at
110 dpi instead of 600. What is on screen is what the encoder will see. A page
costs about 13 ms because rendering a text PDF is parse-bound, not pixel-bound.

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

## Not done

No sandbox entitlements or notarisation -- it is ad-hoc signed and local-only.
Printer discovery is manual: this model answers no mDNS at all, so there is
nothing to browse for and the address is typed in.
