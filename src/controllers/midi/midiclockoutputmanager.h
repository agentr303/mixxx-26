#pragma once

// MidiClockOutputManager
//
// The "front end" of the MIDI clock output feature. Lives on the
// main/GUI thread. Responsibilities:
//
//  1. Watches the user's chosen tempo-source deck (via ControlProxy on
//     "bpm" and "play") and forwards changes into MidiClockGenerator,
//     which is what actually produces sample-accurate tick timings on
//     the audio thread.
//  2. Owns a dedicated std::thread (m_senderThread) that drains
//     MidiClockGenerator's event queue and performs the actual MIDI
//     send to the user's chosen output device at (as close as
//     possible to) the scheduled time.
//  3. Exposes setters used by DlgPrefMidiClock (enabled, output
//     device, tempo source, whether to send transport messages).
//
// This class deliberately does NOT go through the JS controller
// scripting engine. It looks up the already-opened MidiController for
// the user's chosen output device via ControllerManager and calls its
// low-level send method directly, so there is no QJSEngine, event
// loop, or script sandboxing in the send path -- only the OS MIDI
// backend between us and the wire.

#include <QObject>
#include <QString>
#include "control/controlpushbutton.h"
#include "control/controlproxy.h"
#include <memory>
#include <thread>

#include "engine/sync/midiclockgenerator.h"

class ControlProxy;
class ControllerManager;
class MidiController;

class MidiClockOutputManager : public QObject {
    Q_OBJECT

  public:
    explicit MidiClockOutputManager(ControllerManager* pControllerManager);
    ~MidiClockOutputManager() override;

    // Returns the generator so CoreServices can wire process() into
    // the audio callback (EngineMixer). See enginemixer.cpp patch.
    MidiClockGenerator* generator() {
        return &m_generator;
    }

    // Names of currently-connected controllers with an open output
    // that are eligible to receive the clock. Used to populate the
    // preferences page's device dropdown.
    QStringList availableOutputDevices() const;

    // Group strings ("[Channel1]", etc.) of decks currently eligible
    // as a tempo source, i.e. currently loaded with a track. Used to
    // populate the preferences page's tempo-source dropdown. Includes
    // a synthetic "[Master]" entry that tracks the current sync
    // leader's tempo via the internal clock's bpm control.
    QStringList availableTempoSources() const;

  public slots:
    void setEnabled(bool enabled);
    void setOutputDevice(const QString& deviceName);
    void setTempoSourceGroup(const QString& group);
    void setSendTransport(bool sendTransport);

    // Called (see INTEGRATION.md) whenever a MIDI controller finishes
    // opening with a mapping loaded. If the user hasn't already
    // configured an output device, and this mapping's root element
    // carries sendMidiClock="1", this controller becomes the default
    // clock output and the feature turns itself on. Never overrides
    // an explicit choice already made in Preferences.
    void onControllerMappingLoaded(const QString& controllerName, const QString& mappingFilePath);

  private slots:
    void slotSourceBpmChanged(double bpm);
    void slotSourcePlayChanged(double play);

  private:
    void rebuildTempoSourceConnections();
    void senderThreadMain();

    ControllerManager* m_pControllerManager;
    MidiClockGenerator m_generator;

    QString m_outputDeviceName;
    QString m_tempoSourceGroup;
    bool m_enabled;
    // True once setOutputDevice() has ever been called with a
    // non-empty name -- covers both a user's manual dropdown choice
    // and a previously-saved preference loaded at startup. Guards the
    // XML-mapping auto-select in onControllerMappingLoaded() so it
    // only ever fills in a device for a user who has never set one,
    // and never silently overrides an existing choice.
    bool m_deviceExplicitlySet;

    std::unique_ptr<ControlProxy> m_pBpmControl;
    std::unique_ptr<ControlProxy> m_pPlayControl;
    std::unique_ptr<ControlPushButton> m_pEnabledControl;

    std::thread m_senderThread;
    std::atomic<bool> m_stopSenderThread;
};
