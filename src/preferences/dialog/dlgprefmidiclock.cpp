#include "preferences/dialog/dlgprefmidiclock.h"

#include <QFormLayout>
#include <QLabel>

#include "controllers/midi/midiclockoutputmanager.h"

namespace {
const ConfigKey kEnabledKey("[MidiClock]", "Enabled");
const ConfigKey kOutputDeviceKey("[MidiClock]", "OutputDevice");
const ConfigKey kTempoSourceKey("[MidiClock]", "TempoSource");
const ConfigKey kSendTransportKey("[MidiClock]", "SendTransport");
} // namespace

DlgPrefMidiClock::DlgPrefMidiClock(QWidget* pParent,
        UserSettingsPointer pConfig,
        MidiClockOutputManager* pMidiClockOutputManager)
        : DlgPreferencePage(pParent),
          m_pConfig(pConfig),
          m_pMidiClockOutputManager(pMidiClockOutputManager) {
    auto* pLayout = new QVBoxLayout(this);
    auto* pForm = new QFormLayout();

    m_pEnabledCheckbox = new QCheckBox(tr("Enable MIDI clock output"), this);
    pForm->addRow(m_pEnabledCheckbox);

    m_pOutputDeviceCombo = new QComboBox(this);
    pForm->addRow(tr("Output device"), m_pOutputDeviceCombo);

    m_pTempoSourceCombo = new QComboBox(this);
    pForm->addRow(tr("Tempo source"), m_pTempoSourceCombo);

    m_pSendTransportCheckbox =
            new QCheckBox(tr("Send Start / Continue / Stop with deck play state"), this);
    pForm->addRow(m_pSendTransportCheckbox);

    auto* pNote = new QLabel(
            tr("Tip: pick a deck that has Sync enabled as the tempo source so "
               "the clock always reflects the tempo you're mixing to, even "
               "when other decks change BPM."),
            this);
    pNote->setWordWrap(true);

    pLayout->addLayout(pForm);
    pLayout->addWidget(pNote);
    pLayout->addStretch();
    setLayout(pLayout);

    connect(m_pEnabledCheckbox, &QCheckBox::toggled, this, &DlgPrefMidiClock::slotEnabledToggled);
    connect(m_pOutputDeviceCombo,
            &QComboBox::currentTextChanged,
            m_pMidiClockOutputManager,
            &MidiClockOutputManager::setOutputDevice);
    connect(m_pTempoSourceCombo,
            &QComboBox::currentTextChanged,
            m_pMidiClockOutputManager,
            &MidiClockOutputManager::setTempoSourceGroup);
    connect(m_pSendTransportCheckbox,
            &QCheckBox::toggled,
            m_pMidiClockOutputManager,
            &MidiClockOutputManager::setSendTransport);

    slotUpdate();
}

void DlgPrefMidiClock::slotEnabledToggled(bool checked) {
    m_pOutputDeviceCombo->setEnabled(checked);
    m_pTempoSourceCombo->setEnabled(checked);
    m_pSendTransportCheckbox->setEnabled(checked);
    if (checked) {
        refreshDeviceList();
    }
    m_pMidiClockOutputManager->setEnabled(checked);
}

void DlgPrefMidiClock::refreshDeviceList() {
    const QString currentDevice = m_pOutputDeviceCombo->currentText();
    m_pOutputDeviceCombo->clear();
    m_pOutputDeviceCombo->addItems(m_pMidiClockOutputManager->availableOutputDevices());
    const int deviceIdx = m_pOutputDeviceCombo->findText(currentDevice);
    if (deviceIdx >= 0) {
        m_pOutputDeviceCombo->setCurrentIndex(deviceIdx);
    }

    const QString currentSource = m_pTempoSourceCombo->currentText();
    m_pTempoSourceCombo->clear();
    m_pTempoSourceCombo->addItems(m_pMidiClockOutputManager->availableTempoSources());
    const int sourceIdx = m_pTempoSourceCombo->findText(currentSource);
    if (sourceIdx >= 0) {
        m_pTempoSourceCombo->setCurrentIndex(sourceIdx);
    }
}

void DlgPrefMidiClock::slotUpdate() {
    const bool enabled = m_pConfig->getValue(kEnabledKey, false);
    m_pEnabledCheckbox->setChecked(enabled);
    slotEnabledToggled(enabled);

    refreshDeviceList();

    const QString device = m_pConfig->getValue(kOutputDeviceKey, QString());
    const int deviceIdx = m_pOutputDeviceCombo->findText(device);
    if (deviceIdx >= 0) {
        m_pOutputDeviceCombo->setCurrentIndex(deviceIdx);
    }

    const QString source = m_pConfig->getValue(kTempoSourceKey, QString("[Channel1]"));
    const int sourceIdx = m_pTempoSourceCombo->findText(source);
    if (sourceIdx >= 0) {
        m_pTempoSourceCombo->setCurrentIndex(sourceIdx);
    }

    const bool sendTransport = m_pConfig->getValue(kSendTransportKey, true);
    m_pSendTransportCheckbox->setChecked(sendTransport);
}

void DlgPrefMidiClock::slotApply() {
    m_pConfig->setValue(kEnabledKey, m_pEnabledCheckbox->isChecked());
    m_pConfig->setValue(kOutputDeviceKey, m_pOutputDeviceCombo->currentText());
    m_pConfig->setValue(kTempoSourceKey, m_pTempoSourceCombo->currentText());
    m_pConfig->setValue(kSendTransportKey, m_pSendTransportCheckbox->isChecked());

    m_pMidiClockOutputManager->setEnabled(m_pEnabledCheckbox->isChecked());
    m_pMidiClockOutputManager->setOutputDevice(m_pOutputDeviceCombo->currentText());
    m_pMidiClockOutputManager->setTempoSourceGroup(m_pTempoSourceCombo->currentText());
    m_pMidiClockOutputManager->setSendTransport(m_pSendTransportCheckbox->isChecked());
}

void DlgPrefMidiClock::slotResetToDefaults() {
    m_pEnabledCheckbox->setChecked(false);
    m_pSendTransportCheckbox->setChecked(true);
    slotEnabledToggled(false);
    const int sourceIdx = m_pTempoSourceCombo->findText(QStringLiteral("[Channel1]"));
    if (sourceIdx >= 0) {
        m_pTempoSourceCombo->setCurrentIndex(sourceIdx);
    }
}
#include "moc_dlgprefmidiclock.cpp"
