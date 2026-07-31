#pragma once

// mappingRequestsMidiClock()
//
// Borrowed idea: rather than requiring every user to dig into
// Preferences > MIDI Clock Output, a controller mapping author can
// opt a device into receiving the clock by default just by adding an
// attribute to their mapping's root element:
//
//   <MixxxControllerPreset mixxxVersion="2.6+" xmlns="http://mixxx.org">
//       <controller id="YourHardwareDevice" sendMidiClock="1">
//           ...
//       </controller>
//   </MixxxControllerPreset>
//
// This is deliberately implemented as a standalone function that
// parses the mapping file directly with QXmlStreamReader, rather than
// hooking into Mixxx's internal mapping-preset parser. That keeps it
// decoupled from whatever that parser class is called in your
// checkout (it's been renamed more than once -- MidiControllerPreset,
// LegacyMidiControllerMapping, etc.) and from its parsing lifecycle.
// The tradeoff is it re-reads the XML file once per controller at
// connect time (a few KB, happens once, not on any hot path) instead
// of reusing an already-parsed DOM. If you'd rather thread this
// through the real preset object because your checkout already has it
// parsed and in memory, that's a reasonable follow-up simplification
// -- see the NOTE in midiclockoutputmanager.cpp where this is called.
//
// This function only tells you what the mapping *requests*; it does
// not enable anything by itself. MidiClockOutputManager uses it only
// to pick a sensible *default* output device the first time the
// feature is turned on with no device configured yet -- the
// Preferences page's dropdown is still the source of truth, and a
// user's explicit choice there always wins.

#include <QFile>
#include <QXmlStreamReader>

inline bool mappingRequestsMidiClock(const QString& mappingFilePath) {
    QFile file(mappingFilePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    QXmlStreamReader xml(&file);
    while (!xml.atEnd() && !xml.hasError()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("controller")) {
            const QString value = xml.attributes()
                                          .value(QLatin1String("sendMidiClock"))
                                          .toString();
            return value == QLatin1String("1") || value.toLower() == QLatin1String("true");
        }
    }
    return false;
}
