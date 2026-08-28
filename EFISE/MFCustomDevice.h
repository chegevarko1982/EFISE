#pragma once

#include <Arduino.h>
#include "OledMonitorPanel.h"

// only one entry required if you have only one custom device
enum {
    GAGAGU_FCU_EFIS = 1
};
class MFCustomDevice
{
public:
    MFCustomDevice();
    void attach(uint16_t adrPin, uint16_t adrType, uint16_t adrConfig, bool configFromFlash);
    void detach();
    void update();
    void set(int16_t messageID, char *setPoint);

private:
    bool           getStringFromMem(uint16_t addrMem, char *buffer, bool configFromFlash);
    bool           _initialized = false;
    OledMonitorPanel       *_panel;
    uint8_t        _customType = 0;
};
