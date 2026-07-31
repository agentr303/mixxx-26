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
const QString kMasterTempoGroup = QStringLiteral("[InternalClock]");
const QString kMasterTempoKey = QStringLiteral("bpm");
const QString kMasterSourceName = QStringLiteral("[Master]");

const QString kPlayKey = QStringLiteral("play");
const QString kBpmKey = QStringLiteral("bpm");

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
#if defined(Q_OS_LINUX) || defined(Q_OS_MAC)
    QThread::currentThread()->setPriority(QThread::TimeCriticalPriority);
#endif

    MidiClockQueue::Event event;
    while (!m_stopSenderThread.load(std::memory_order_relaxed)) {
        MidiClockQueue* pQueue = m_generator.queue();
        if (!pQueue->pop(&event)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

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

        const unsigned char byte = event.byte;
        QMetaObject::invokeMethod(
                pTarget,
                [pTarget, byte]() { pTarget->sendRealTimeByte(byte); },
                Qt::QueuedConnection);
    }
}
