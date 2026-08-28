#pragma once

#include "Arduino.h"
#include "OLEDInterface.h"

/*
  Multiplexer channel the one display sits behind.

  Zero, which is what the original firmware this board runs uses for EFIS
  left - NOT the 1 the eight-screen panel remapped it to. Whether a
  TCA9548A is actually fitted does not matter: setTCAChannel() caches the
  channel, so this costs one I2C write for the life of the boot, and if
  nothing answers at that address the write is a harmless NAK which
  attach() reports once.
*/
#define TCA9548A_CHANNEL_BARO 0

// Screen indices used for the dirty-bit mask in OledMonitorPanel::_dirty.
// These are NOT TCA channel numbers - the TCA9548A_CHANNEL_* macros above
// stay the values passed to setTCAChannel().
enum : uint8_t {
    SCR_BARO,
    SCR_COUNT
};

/*
  Animation opt-in, one bit per SCR_* index - deliberately the same bit
  layout as _dirty, so "every screen" is 0xFF and adding a screen later
  costs nothing here. Built by MFCustomDevice::attach() from the Config
  string in the board settings; 0 (an empty Config string) means the panel
  behaves exactly as it did before animation existed.
*/
#define ANIM_BARO       (1 << SCR_BARO)

// The only screen there is. Kept as a separate name so the Config string
// reads the same as on the eight-screen panel.
#define ANIM_ALL        ANIM_BARO

// Frame count bounds accepted from the Config string, and the default when
// it names screens but no FRAMES=.
// The baro field is four digits, in both hPa (1013) and inHg (2992, sent
// without its dot). _shadow[] keeps a fifth byte for its NUL terminator; the
// drum arrays do not need one.
#define MAX_DIGIT_CELLS 4

#define ANIM_FRAMES_MIN     2
#define ANIM_FRAMES_MAX     8
#define ANIM_FRAMES_DEFAULT 8

class OledMonitorPanel
{
public:
    OledMonitorPanel();
    void begin();
    void attach(uint8_t addrI2C, uint8_t animMask = 0, uint8_t frames = ANIM_FRAMES_DEFAULT);
    void detach();
    void set(int16_t messageID, char *message);
    void update();

private:
    bool          _initialised;
    uint8_t       _addrI2C;
    uint8_t       _currentChannel;
    uint8_t       _dirty;
    uint8_t       _animMask;    // ANIM_* bits; 0 = animation off (default)
    uint8_t       _animFrames;  // frames per slide, ANIM_FRAMES_MIN..MAX
    OLEDInterface *oled;

    // Baseline (pixel row) every DSEG7 digit cell on this panel is drawn on.
    // Fixed by the physical layout, not per-call - so it is a constant, not
    // a parameter.
    // 62, not the 60 the original firmware used. A cell has to be whole
    // framebuffer pages, and the tallest 20pt glyph is 39 rows: at baseline 60
    // it inks rows 22..60, which spans six pages (16..63) and leaves nine rows
    // of the cell blank at rest. At 62 it inks 24..62 - exactly five pages
    // (24..63). That is a sixth less I2C per redraw, one dead row instead of
    // nine while a digit rolls, and it lifts the cell clear of the QFE/QNH
    // label, which inks down to row 16 and previously shared row 16 with it.
    static const uint8_t DIGIT_BASELINE_Y = 62;

    // Partial-redraw shadow state - what is currently sitting in the
    // framebuffer for each screen, so renderCells() only has to touch the
    // digit cells that actually changed. See renderCells()/commitCells().
    //
    // The invariant is one-directional and easy to break: _shadow describes
    // what is ON THE PANEL, not what the value happens to be. Any branch that
    // paints a screen some other way (light test, managed dashes, a slide in
    // flight) must either commit the new contents or invalidate _shadowSig -
    // leaving a stale entry alone is what makes a screen quietly stop
    // updating, because the next value compares equal to it and nothing is
    // drawn.
    char    _shadow[SCR_COUNT][MAX_DIGIT_CELLS + 1]; // digits as last drawn, NUL-terminated
    uint8_t _shadowSig[SCR_COUNT];    // layout signature of what is on screen; 0 = unknown

    // Odometer drum state, one entry per digit cell.
    //
    // A cell is a wheel carrying 0..9, and _drumPos is where that wheel has
    // turned to - Q8 in digit units, so 0x0250 is "half way between 2 and 3".
    // Everything follows from keeping a position rather than a from/to pair:
    // a value arriving mid-roll only moves _drumTarget, and the wheel carries
    // on from where it is. Nothing is ever interrupted, which is the whole
    // point - the previous fixed-length slide had to abort on every value that
    // landed inside its 96 ms, and an aborted roll is exactly what a snapped
    // digit looks like.
    //
    // Cells roll concurrently and drumStep() advances exactly one per call -
    // 7.1 ms measured, the longest stretch this firmware blocks serial for.
    // _drumCursor takes them in turn so no screen can starve the others.
    uint16_t _drumPos[SCR_COUNT][MAX_DIGIT_CELLS];    // Q8 digit units, 0..DRUM_SPAN-1
    uint8_t  _drumTarget[SCR_COUNT][MAX_DIGIT_CELLS]; // digit 0..9 each cell is turning to
    uint8_t  _drumMoving[SCR_COUNT];                  // bit per cell still turning
    uint8_t  _drumActive;                             // bit per screen with any cell turning
    uint8_t  _drumUp;                                 // bit per screen: wheel turns upward
    uint8_t  _drumCursor;                             // round-robin position over screens
    uint32_t _lastFrameMs;                            // millis() at the last cell advanced

    bool _busFaultReported;     // a silent multiplexer is reported once, not per transaction
    bool _busTimeoutReported;   // likewise for a bus that stalls mid-transaction

    void setTCAChannel(byte i);
    void blankAllDisplays(void);
    void renderScreen(uint8_t scr);
    void printCentered(const char *text, int16_t y);
    void renderLabelValue(byte channel,
                           const char *labelText, int16_t labelY, const GFXfont *labelFont,
                           const char *valueText, int16_t valueX, int16_t valueY, const GFXfont *valueFont,
                           bool drawDot, int16_t dotX, int16_t dotY);
    void fastDrawDigit(uint8_t cursorX, uint8_t page0, uint8_t pages,
                        const GFXfont *font, char c, int16_t yShift = 0);
    void clearCell(uint8_t blitX, uint8_t page0, uint8_t pages, uint8_t blitW);
    bool renderCells(uint8_t scr, const char *cells, uint8_t sig);
    void commitCells(uint8_t scr, const char *cells, uint8_t sig);
    bool    recoverI2CBus(void);
    bool    slideCells(uint8_t scr, const char *cells, uint8_t sig);
    uint8_t cellsInFlight(void) const;
    void    drumStep(void);
    void abortDrum(uint8_t scr);
    void updateDisplayBaro(void);

};
