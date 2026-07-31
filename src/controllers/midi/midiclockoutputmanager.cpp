#include "controllers/midi/midiclockoutputmanager.h"

#include <QMetaObject>
#include <QThread>
#include <QtDebug>
#include <thread>

#include "control/controlproxy.h"
#include "controllers/controllermanager.h"
#include "controllers/midi/midiclockmappingflag.h"
#include "controllers/midi/midicontroller.h"
#include "control/controlpushbutton.h"

namespace {
// NOTE ON THE "[Master]" TEMPO SOURCE ENTRY:
// EngineSync keeps "[InternalClock]", "bpm" tracking the current sync
// leader's tempo whenever any deck is leading, and the internal
// clock's own tempo when nothing is (e.g. all decks synced but
// stopped, or the internal clock itself is the leader). This makes it
// a convenient always-available "current session tempo" source. If
// your checkout's control key differs, update kMasterTempoGroup /
// kMasterTempoKey below -- this is the one place in this file that is
// most likely to need adjustment across Mixxx versions.
const QString kMasterTempoGroup = QStringLiteral("[InternalClock]");
const QString kMasterTempoKey = QStringLiteral("bpm");
const QString kMasterSourceName = QStringLiteral("[Master]");

const QString kPlayKey = QStringLiteral("play");
const QString kBpmKey = QStringLiteral("bpm");

// How long before a scheduled tick's timestamp we wake up to prepare
// the send. Waking early and spin-waiting the last stretch trades a
// little CPU for much better precision than relying solely on OS
// sleep granularity (which is commonly 1-15 ms depending on platform
// and power state).
constexpr auto kWakeAheadMargin = std::chrono::microseconds(800);
constexpr auto kSpinThreshold = std::chrono::microseconds(200);
} // namespace

MidiClockOutputManager::MidiClockOutputManager(ControllerManager* pControllerManager)
        : m_pControllerManager(pControllerManager),
          m_enabled(false),
          m_deviceExplicitlySet(false),
          m_stopSenderThread(false) {
    m_senderThread = std::thread(&MidiClockOutputManager::senderThreadMain, this);
                m_pEnabledControl = std::make_unique<ControlPushButton>(ConfigKey("[MidiClock]", "enabled"));
       m_pEnabledControl->setButtonMode(mixxx::control::ButtonMode::Toggle);
       connect(m_pEnabledControl.get(), &ControlObject::valueChanged, this, [this](double value) {
       setEnabled(value > 0.0);
   });
}

MidiClockOutputManager::~MidiClockOutputManager() {
    m_stopSenderThread.store(true, std::memory_order_relaxed);
    if (m_senderThread.joinable()) {
        m_senderThread.join();
    }
}

QStringList MidiClockOutputManager::availableOutputDevices() const {
    QStringList result;
    if (!m_pControllerManager) {
        return result;
    }
    // NOTE: getControllerList()/isOpen() reflect the API at the time
    // this feature was written. If ControllerManager's accessor names
    // differ in your checkout, adjust this loop accordingly -- the
    // logic (list controllers, keep the MIDI ones with an open output)
    // stays the same.
    const auto controllers = m_pControllerManager->getControllerList();
    for (auto* pController : controllers) {
        auto* pMidiController = qobject_cast<MidiController*>(pController);
        if (pMidiController && pMidiController->isOpen()) {
            result << pMidiController->getName();
        }
    }
    return result;
}

QStringList MidiClockOutputManager::availableTempoSources() const {
    QStringList result;
    result << kMasterSourceName;
    // Decks: adjust the range/group naming if your build supports a
    // different deck count or naming scheme (e.g. more than 4 decks).
    for (int i = 1; i <= 4; ++i) {
        result << QStringLiteral("[Channel%1]").arg(i);
    }
    return result;
}

void MidiClockOutputManager::setEnabled(bool enabled) {
    m_enabled = enabled;
    m_generator.setEnabled(enabled);
    if (enabled) {
        rebuildTempoSourceConnections();
    }
}

void MidiClockOutputManager::setOutputDevice(const QString& deviceName) {
    m_outputDeviceName = deviceName;
    if (!deviceName.isEmpty()) {
        m_deviceExplicitlySet = true;
    }
    m_generator.requestRealign();
}

void MidiClockOutputManager::onControllerMappingLoaded(
        const QString& controllerName, const QString& mappingFilePath) {
    if (m_deviceExplicitlySet || mappingFilePath.isEmpty()) {
        return;
    }
    if (mappingRequestsMidiClock(mappingFilePath)) {
        qInfo() << "MidiClockOutputManager: mapping for" << controllerName
                << "requested sendMidiClock=\"1\"; using it as the default clock output";
        setOutputDevice(controllerName);
        if (m_tempoSourceGroup.isEmpty()) {
            setTempoSourceGroup(QStringLiteral("[Master]"));
        }
        setEnabled(true);
    }
}

void MidiClockOutputManager::setTempoSourceGroup(const QString& group) {
    m_tempoSourceGroup = group;
    rebuildTempoSourceConnections();
    m_generator.requestRealign();
}

void MidiClockOutputManager::setSendTransport(bool sendTransport) {
    m_generator.setSendTransport(sendTransport);
}

void MidiClockOutputManager::rebuildTempoSourceConnections() {
    m_pBpmControl.reset();
    m_pPlayControl.reset();

    if (m_tempoSourceGroup.isEmpty()) {
        return;
    }

    const QString group = (m_tempoSourceGroup == kMasterSourceName)
            ? kMasterTempoGroup
            : m_tempoSourceGroup;

    m_pBpmControl = std::make_unique<ControlProxy>(group, kBpmKey, this);
    m_pBpmControl->connectValueChanged(this, &MidiClockOutputManager::slotSourceBpmChanged);

    if (m_tempoSourceGroup == kMasterSourceName) {
        // The synthetic "Master" source has no single deck play
        // state; treat the clock as always "playing" and rely on the
        // Start/Stop/Continue toggle in preferences if the user wants
        // transport messages suppressed while nothing is playing.
        // A more complete implementation could OR together every
        // deck's [ChannelN] play control here.
        m_generator.setPlaying(true);
    } else {
        m_pPlayControl = std::make_unique<ControlProxy>(group, kPlayKey, this);
        m_pPlayControl->connectValueChanged(this, &MidiClockOutputManager::slotSourcePlayChanged);
        m_generator.setPlaying(m_pPlayControl->get() > 0.0);
    }

    if (m_pBpmControl) {
        slotSourceBpmChanged(m_pBpmControl->get());
    }
}

void MidiClockOutputManager::slotSourceBpmChanged(double bpm) {
    m_generator.setBpm(bpm);
}

void MidiClockOutputManager::slotSourcePlayChanged(double play) {
    m_generator.setPlaying(play > 0.0);
}

void MidiClockOutputManager::senderThreadMain() {
    // This thread does two things, deliberately kept separate from
    // both the audio thread (which must never block) and the GUI
    // thread (which must never be blocked waiting on us): it sleeps
    // until each queued tick's scheduled time, then hands the byte
    // off to the selected MidiController.
    //
    // Raise this thread's priority if the platform allows it -- a
    // clock thread that gets pre-empted by, say, a library scan is
    // exactly the jitter source this feature exists to avoid.
#if defined(Q_OS_LINUX) || defined(Q_OS_MAC)
    // Best-effort; requires appropriate privileges/rlimits on Linux.
    // Silently no-ops if unavailable.
    QThread::currentThread()->setPriority(QThread::TimeCriticalPriority);
#endif

    MidiClockQueue::Event event;
    while (!m_stopSenderThread.load(std::memory_order_relaxed)) {
        MidiClockQueue* pQueue = m_generator.queue();
        if (!pQueue->pop(&event)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // Sleep until (timestamp - margin), then spin the last little
        // bit for precision. std::this_thread::sleep_until is used
        // rather than sleep_for to avoid accumulating drift from the
        // time spent handling the previous event.
        const auto wakeTime = event.timestamp - kWakeAheadMargin;
        if (wakeTime > std::chrono::steady_clock::now()) {
            std::this_thread::sleep_until(wakeTime);
        }
        while (event.timestamp - std::chrono::steady_clock::now() > kSpinThreshold) {
            std::this_thread::yield();
        }
        while (std::chrono::steady_clock::now() < event.timestamp) {
            // Tight spin for the final sub-200us stretch.
        }

        if (!m_pControllerManager || m_outputDeviceName.isEmpty()) {
            continue;
        }
        const auto controllers = m_pControllerManager->getControllerList();
        MidiController* pTarget = nullptr;
        for (auto* pController : controllers) {
            auto* pMidiController = qobject_cast<MidiController*>(pController);
            if (pMidiController && pMidiController->getName() == m_outputDeviceName) {
                pTarget = pMidiController;
                break;
            }
        }
        if (!pTarget) {
            continue;
        }

        // Marshal the actual send onto the controller's own thread via
        // a queued connection, since MidiController I/O is not
        // guaranteed thread-safe to call directly from an arbitrary
        // thread. On an idle controller thread this adds well under a
        // millisecond; if your checkout's MidiController exposes a
        // send method that IS documented safe to call from any
        // thread, calling it directly here removes that margin.
        const unsigned char byte = event.byte;
        QMetaObject::invokeMethod(
