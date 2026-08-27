#pragma once

#include <juce_core/juce_core.h>

#include <functional>

// The IR library's non-GUI backbone, ported from basilica-audio/Nave (issue
// #111 here, #45 there): pure path computations plus a directory scan, kept
// free of any juce_gui_basics dependency so tests can exercise it headlessly.
//
// CRYPTA HAS NO IR BROWSER (yet - the GUI's IR slot list renders the four
// factory cabinets and "None", nothing file-based). What it does have since
// #111 is preset -> IR references that resolve BY CONTENT HASH against the
// user's library folder first and the embedded bundle second (see
// src/ir/BundledIrSource.h), so the library folder and the scan exist for the
// resolution side alone. Nave's background scanner thread (IrLibraryScanner)
// is deliberately NOT ported: it is browser machinery, and carrying it dormant
// here would be dead code wearing a threading contract.
//
// THREADING. Everything here is file-system I/O: message thread (or a
// background thread), never processBlock().
namespace basilica::ir
{
    namespace IrLibrary
    {
        // Upper bound on how many files a single scan collects. Guards the
        // scan against a user pointing the library folder at, say, their
        // entire home directory - the cap keeps worst-case scan cost and
        // memory bounded and predictable.
        inline constexpr int defaultMaxFiles = 2000;

        // The out-of-the-box library folder preset references resolve
        // against: <user music dir>/Crypta/Impulse Responses (e.g.
        // ~/Music/Crypta/Impulse Responses on macOS). Deliberately NOT
        // created here - defaultDirectory() is a pure path computation, and
        // a scan of a not-yet-existing folder simply yields an empty list.
        // Nothing in Crypta ever writes into this folder unasked; a user who
        // keeps IR files here gets them preferred over the embedded copies
        // (see BundledIrSource.h's precedence note).
        juce::File defaultDirectory();

        // Where Crypta writes its own embedded IRs out to disk so that a
        // preset reference can resolve against them without any install step
        // (see src/ir/BundledIrSource.h):
        // <user app data>/Basilica Audio/Crypta/Bundled Impulse Responses
        // (on macOS, <user app data> is ~/Library/Application Support).
        //
        // DELIBERATELY NOT defaultDirectory(). Writing there would put files
        // the user never chose into their own library folder - the silent
        // install src/ir/FactoryIrLibrary.h refuses to perform. This location
        // is NEVER a search root: it is a reconstructible cache of bytes that
        // are already inside the binary, not a library. Deleting it costs
        // nothing - the next reference that needs a file re-creates it.
        //
        // Like defaultDirectory(), a pure path computation that creates
        // nothing.
        juce::File bundledCacheDirectory();

        // True for the audio-file types an IR slot can actually load
        // (WAV/AIFF), matched case-insensitively.
        bool isImpulseResponseFile (const juce::File& file);

        // Recursively collects every IR file under `root` (skipping hidden
        // entries), sorted case-insensitively by full path so the listing
        // order is deterministic across platforms and repeat scans, capped
        // at `maxFiles`. A non-existent/non-directory `root` yields an
        // empty result. `shouldAbort` (optional) is polled once per
        // directory entry; an aborted scan returns an empty (never partial)
        // result.
        //
        // Synchronous and blocking - call it from the message thread or a
        // test, never from the audio thread.
        juce::Array<juce::File> scan (const juce::File& root,
                                      int maxFiles = defaultMaxFiles,
                                      const std::function<bool()>& shouldAbort = {});
    }
}
