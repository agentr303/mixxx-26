#pragma once

// DlgPrefMidiClock
//
// Preferences > MIDI Clock Output page. Lets the user:
//  - turn the feature on/off
//  - pick which connected MIDI controller's output receives the clock
//  - pick which deck's tempo (or "[Master]"/sync leader) drives it
//  - decide whether Start/Continue/Stop transport bytes are sent
//
// Built up programmatically (no .ui file) to keep this patch
// self-contained; feel free to move this to a Designer .ui form to
// match the styling of Mixxx's other preferences pages.
//
// NOTE: This assumes the DlgPreferencePage interface used by Mixxx's
// other preferences pages (slotApply / slotUpdate / slotResetToDefaults).
// Check src/preferences/dialog/dlgpreferencepage.h in your checkout and
// adjust the base class / overrides if the signatures differ.

#include <QCheckBox>
#include <QComboBox>
#include <QVBoxLayout>

#include "preferences/dialog/dlgpreferencepage.h"
#include "preferences/usersettings.h"

class MidiClockOutputManager;

class DlgPrefMidiClock : public DlgPreferencePage {
    Q_OBJECT

  public:
    DlgPrefMidiClock(QWidget* pParent,
            UserSettingsPointer pConfig,
            MidiClockOutputManager* pMidiClockOutputManager);
    ~DlgPrefMidiClock() override = default;

  public slots:
    void slotApply() override;
    void slotUpdate() override;
    void slotResetToDefaults() override;

  private slots:
    void slotEnabledToggled(bool checked);
    void refreshDeviceList();

  private:
    UserSettingsPointer m_pConfig;
    MidiClockOutputManager* m_pMidiClockOutputManager;

    QCheckBox* m_pEnabledCheckbox;
    QComboBox* m_pOutputDeviceCombo;
    QComboBox* m_pTempoSourceCombo;
    QCheckBox* m_pSendTransportCheckbox;
};
