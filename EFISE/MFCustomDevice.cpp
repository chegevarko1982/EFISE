#include "MFCustomDevice.h"
#include "commandmessenger.h"
#include "allocateMem.h"
#include "MFEEPROM.h"
#if defined(HAS_CONFIG_IN_FLASH)
#include "MFCustomDevicesConfig.h"
#else
const char CustomDeviceConfig[] PROGMEM = {};
#endif

extern MFEEPROM MFeeprom;

/* **********************************************************************************
    The custom device pins, type and configuration is stored in the EEPROM
    While loading the config the adresses in the EEPROM are transferred to the constructor
    Within the constructor you have to copy the EEPROM content to a buffer
    and evaluate him. The buffer is used for all 3 types (pins, type configuration),
    so do it step by step.
    The max size of the buffer is defined here. It must be the size of the
    expected max length of these strings.

    E.g. 6 pins are required, each pin could have two characters (two digits),
    each pins are delimited by "|" and the string is NULL terminated.
    -> (6 * 2) + 5 + 1 = 18 bytes is the maximum.
    The custom type is "MyCustomClass", which means 14 characters plus NULL = 15
    The configuration is "myConfig", which means 8 characters plus NULL = 9
    The maximum characters to be expected is 18, so MEMLEN_STRING_BUFFER has to be at least 18
********************************************************************************** */
#define MEMLEN_STRING_BUFFER 40

// reads a string from EEPROM or Flash at given address which is '.' terminated and saves it to the buffer
bool MFCustomDevice::getStringFromMem(uint16_t addrMem, char *buffer, bool configFromFlash)
{
    char     temp   = 0;
    uint8_t  counter = 0;
    uint16_t length  = MFeeprom.get_length();
    do {
        if (configFromFlash) {
            temp = pgm_read_byte_near(CustomDeviceConfig + addrMem++);
            if (addrMem > sizeof(CustomDeviceConfig))
                return false;
        } else {
            temp = MFeeprom.read_byte(addrMem++);
            if (addrMem > length)
                return false;
        }
        buffer[counter++] = temp;               // save character and locate next buffer position
        if (counter >= MEMLEN_STRING_BUFFER) {  // nameBuffer will be exceeded
            return false;                       // abort copying to buffer
        }
    } while (temp != '.');       // reads until limiter '.' and locates the next free buffer position
    buffer[counter - 1] = 0x00;  // replace '.' by NULL, terminates the string
    return true;
}

/* **********************************************************************************
    Helpers for the optional "Additional Config" string of the custom device.
    Syntax: ANIM=<RA|DME|MACH|SPD|HDG|ALT|VS|CRS|ALL|OFF>[+...]|FRAMES=<2..8>
    Key order does not matter, keys and values are case insensitive.
    An empty or absent config string means animation off.

    The list separator is '+' and NOT ',' - that is not a style choice. ',' is
    the CmdMessenger field separator, so a comma typed into the settings field
    is cut off there and never reaches the EEPROM: the board ends up storing a
    config field with no terminating '.', Config.cpp's readEndCommand() then
    runs past the end of the entry, CustomDevice::Add() is never called and the
    whole custom device silently disappears from the board. ';' (command
    separator), '/' (escape), and '.' / ':' (EEPROM field and device
    terminators) are unusable for the same class of reason.
********************************************************************************** */

// strips leading and trailing spaces, modifies the buffer in place
static char *trimSpaces(char *s)
{
    while (*s == ' ')
        s++;
    char *end = s;
    while (*end)
        end++;
    while (end > s && *(end - 1) == ' ')
        *(--end) = 0x00;
    return s;
}

// parses a plain decimal number and checks it against the allowed frame range
static bool parseFrames(const char *s, uint8_t *frames)
{
    if (*s == 0x00) return false;
    uint16_t value = 0;
    while (*s) {
        if (*s < '0' || *s > '9') return false;
        value = value * 10 + (uint8_t)(*s - '0');
        if (value > ANIM_FRAMES_MAX) return false;
        s++;
    }
    if (value < ANIM_FRAMES_MIN) return false;
    *frames = (uint8_t)value;
    return true;
}

// parses the '+' separated screen list of the ANIM key into a SCR_* bit mask
static bool parseAnimList(char *list, uint8_t *mask)
{
    char *state = NULL;
    char *token = strtok_r(list, "+", &state);
    if (token == NULL) return false; // "ANIM=" without any value
    while (token != NULL) {
        token = trimSpaces(token);
        // the _P variants keep the keywords in flash instead of RAM
        if (strcasecmp_P(token, PSTR("BARO")) == 0)
            *mask |= ANIM_BARO;
        else if (strcasecmp_P(token, PSTR("ALL")) == 0)
            *mask |= ANIM_ALL;
        else if (strcasecmp_P(token, PSTR("OFF")) != 0) // "OFF" adds no bits
            return false;
        token = strtok_r(NULL, "+", &state);
    }
    return true;
}

MFCustomDevice::MFCustomDevice()
{
    _initialized = false;
}

/* **********************************************************************************
    Within the connector pins, a device name and a config string can be defined
    These informations are stored in the EEPROM like for the other devices.
    While reading the config from the EEPROM this function is called.
    It is the first function which will be called for the custom device.
    If it fits into the memory buffer, the constructor for the customer device
    will be called
********************************************************************************** */

void MFCustomDevice::attach(uint16_t adrPin, uint16_t adrType, uint16_t adrConfig, bool configFromFlash)
{
    if (adrPin == 0) return;

    /* **********************************************************************************
        Do something which is required to setup your custom device
    ********************************************************************************** */

    char   *params, *p = NULL;
    char    parameter[MEMLEN_STRING_BUFFER];
    uint8_t _addrI2C;

    /* **********************************************************************************
        Read the Type from the EEPROM, copy it into a buffer and evaluate it
        The string get's NOT stored as this would need a lot of RAM, instead a variable
        is used to store the type
    ********************************************************************************** */
    getStringFromMem(adrType, parameter, configFromFlash);
    if (strcmp(parameter, "GAGAGU_FCU-EFIS") == 0)
        _customType = GAGAGU_FCU_EFIS;

    if (_customType == GAGAGU_FCU_EFIS) {
        /* **********************************************************************************
            Check if the device fits into the device buffer
        ********************************************************************************** */
        if (!FitInMemory(sizeof(OledMonitorPanel))) {
            // Error Message to Connector
            cmdMessenger.sendCmd(kStatus, F("Custom Device does not fit in Memory"));
            return;
        }
        /* **********************************************************************************************
            Read the pins from the EEPROM, copy them into a buffer
            If you have set '"isI2C": true' in the device.json file, the first value is the I2C address
        ********************************************************************************************** */
        getStringFromMem(adrPin, parameter, configFromFlash);
        /* **********************************************************************************************
            Split the pins up into single pins. As the number of pins could be different between
            multiple devices, it is done here.
        ********************************************************************************************** */
        params = strtok_r(parameter, "|", &p);
        _addrI2C  = atoi(params);

        /* **********************************************************************************
            Read the configuration from the EEPROM, copy it into a buffer and evaluate it.
            The pin string above is already evaluated into _addrI2C, so the buffer can be
            reused here. An empty or missing config string keeps animation switched off,
            which is the behaviour of all boards configured before this option existed.
        ********************************************************************************** */
        uint8_t _animMask   = 0;
        uint8_t _animFrames = ANIM_FRAMES_DEFAULT;

        parameter[0] = 0x00;
        if (adrConfig > 0 && getStringFromMem(adrConfig, parameter, configFromFlash) && parameter[0] != 0x00) {
            uint8_t mask   = 0;
            uint8_t frames = ANIM_FRAMES_DEFAULT;
            bool    valid  = true;

            params = strtok_r(parameter, "|", &p);
            while (params != NULL && valid) {
                char *key = trimSpaces(params);
                if (*key != 0x00) { // ignore empty sections like "ANIM=RA||FRAMES=4"
                    if (strncasecmp_P(key, PSTR("ANIM="), 5) == 0)
                        valid = parseAnimList(key + 5, &mask);
                    else if (strncasecmp_P(key, PSTR("FRAMES="), 7) == 0)
                        valid = parseFrames(trimSpaces(key + 7), &frames);
                    else
                        valid = false; // unknown key
                }
                params = strtok_r(NULL, "|", &p);
            }

            if (valid) {
                _animMask   = mask;
                _animFrames = frames;
            } else {
                // fall back to animation off, a silently half applied option is worse
                // No ',' in this text either - it would be split into three fields on
                // the way to the connector, for exactly the reason above.
                cmdMessenger.sendCmd(kStatus, F("Custom Device: bad Config - use ANIM=BARO|FRAMES=4"));
            }
        }

        /* **********************************************************************************
            Next call the constructor of your custom device
            adapt it to the needs of your constructor
        ********************************************************************************** */
        // In most cases you need only one of the following functions
        // depending on if the constuctor takes the variables or a separate function is required
        _panel = new (allocateMemory(sizeof(OledMonitorPanel))) OledMonitorPanel();
        _panel->attach(_addrI2C, _animMask, _animFrames);
        // if your custom device does not need a separate begin() function, delete the following
        // or this function could be called from the custom constructor or attach() function
        _panel->begin();
        _initialized = true;
    } else {
        cmdMessenger.sendCmd(kStatus, F("Custom Device is not supported by this firmware version"));
    }
}

/* **********************************************************************************
    The custom devives gets unregistered if a new config gets uploaded.
    Keep it as it is, mostly nothing must be changed.
    It gets called from CustomerDevice::Clear()
********************************************************************************** */
void MFCustomDevice::detach()
{
    _initialized = false;
    if (_customType == GAGAGU_FCU_EFIS) {
        _panel->detach();
    }
}

/* **********************************************************************************
    Within in loop() the update() function is called regularly
    Within the loop() you can define a time delay where this function gets called
    or as fast as possible. See comments in loop().
    It is only needed if you have to update your custom device without getting
    new values from the connector.
    Otherwise just make a return; in the calling function.
    It gets called from CustomerDevice::update()
********************************************************************************** */
void MFCustomDevice::update()
{
    if (!_initialized) return;
    /* **********************************************************************************
        Do something if required
    ********************************************************************************** */
    if (_customType == GAGAGU_FCU_EFIS) {
        _panel->update();
    }
}

/* **********************************************************************************
    If an output for the custom device is defined in the connector,
    this function gets called when a new value is available.
    It gets called from CustomerDevice::OnSet()
********************************************************************************** */
void MFCustomDevice::set(int16_t messageID, char *setPoint)
{
    if (!_initialized) return;

    if (_customType == GAGAGU_FCU_EFIS) {
        _panel->set(messageID, setPoint);
    }
}
