#include "OledMonitorPanel.h"
#include "Fonts/FreeSans9pt7b.h"
#include "Fonts/DSEG7Classic_Regular20pt7b.h" //https://github.com/keshikan/DSEG and https://rop.nl/truetype2gfx/
#include "Fonts/DSEG7Classic_Regular22pt7b.h"
/*
  The whole state of this device: one EFIS baro screen.

  Message ids are the original firmware's and must stay so - a MobiFlight
  project binds to them, and renaming or renumbering would make every output
  on this board need rebinding.
*/
static uint8_t baroSelect = 0x00;         // id 0: 0 = inHg, 1 = hPa
static char    baroValueHpa[6] = "0000";  // id 1: as sent, e.g. 1013
static char    baroValueHg[6]  = "0000";  // id 2: as sent WITHOUT the dot, e.g. 2992
static uint8_t baroMode        = 0x01;    // id 3: 0 = QFE, 1 = QNH, 2 and 3 = STD
static uint8_t lightTestOn     = 0x00;    // id 19

/*
  Bounded copy: copies src into dst (a buffer of dstSize bytes),
  always NUL-terminating, never writing past dst[dstSize-1].
*/
static void copyValue(char *dst, uint8_t dstSize, const char *src)
{
    strncpy(dst, src, dstSize - 1);
    dst[dstSize - 1] = '\0';
}

/*
  Right-align src into dst, zero-padded on the left, producing exactly
  width chars + NUL. If src is longer than width, the rightmost width
  chars are kept.
*/
static void padLeft(char *dst, uint8_t width, const char *src)
{
    uint8_t len = strlen(src);
    if (len > width) len = width; // keep the RIGHTMOST width chars
    memset(dst, '0', width - len);
    memcpy(dst + width - len, src + strlen(src) - len, len);
    dst[width] = '\0';
}

/*
  Copies src into dst (see copyValue) only if it actually differs.
  Returns true if the value changed (and therefore dst was updated).
*/
static bool updateValue(char *dst, uint8_t dstSize, const char *src)
{
    if (strncmp(dst, src, dstSize - 1) == 0) return false;
    copyValue(dst, dstSize, src);
    return true;
}

/*
  Drum tuning, off the bench numbers.

  One cell's frame - clear the rectangle, rasterise the two visible glyphs
  into it, push it once - measured 7.1 ms on this board. drumStep() advances
  exactly one cell per call, so 7.1 ms is also the longest stretch this
  firmware blocks serial for, whatever else is turning. At 115200 baud the
  256-byte receive buffer holds 22 ms, so that leaves a wide margin.

  The 12 ms period follows: 7.1 of 12 ms is 59 % duty, and a one-digit turn
  lands in roughly 8 steps, so about 96 ms - the roll duration the panel was
  tuned to, in steps a quarter the size of the old four-frame slide.

  DRUM_DECEL_SHIFT gives the wheel its feel: each step closes a quarter of
  what is left, so it leaves fast and settles softly, the way a drum on a
  shaft does. DRUM_MIN_STEP stops that tail from crawling - without it the
  last few 1/256ths would take as long as the whole turn. There is no easing
  table and no frame counter: a wheel that is retargeted mid-turn simply has
  further to go, which is why fast input spins it instead of stuttering.
*/
#define ANIM_FRAME_MS      12
#define DRUM_DECEL_SHIFT   2

/*
  Ceiling on how far a wheel may turn in one step, in 1/256ths of a digit.
  Re-aiming a wheel that is already turning makes the remaining distance
  larger, and the proportional step larger with it - which is what makes a
  spun knob spin the drum instead of stuttering. Without a ceiling a big jump
  would move most of a digit in one 12 ms step and read as a teleport rather
  than as motion. 128 is half a digit per step, so a whole revolution takes
  about 20 steps to catch up. It never binds on the ordinary one- or
  two-digit change, whose first step is 64 and 128.
*/
#define DRUM_MAX_STEP      128

// A digit wheel carries 0..9; positions are Q8 in digit units.
#define DRUM_DIGITS 10
#define DRUM_SPAN   (DRUM_DIGITS * 256)

/*
  How many digits a single transition may move and still be turned rather than
  snapped. Five is every cell of the widest screen, so this no longer refuses
  anything - it stays as a bound on the loops below rather than as a policy.

  It was a policy, and the reasoning was wrong twice over. Counting a screen
  one step at a time, a leading digit NEVER changes on its own: 099 -> 100
  moves all three, 09900 -> 10000 moves all five. Capping below the width of
  the screen therefore does not reject jumps - it rejects exactly the carries,
  which are the transitions most worth watching. And the cost it was guarding
  against no longer grows with the number of cells: see the step floor in
  drumStep(), which spends a roughly fixed budget however many are turning.
*/
#define ANIM_MAX_CELLS MAX_DIGIT_CELLS

/*
  How many digit cells may be turning at once across the whole panel.

  Twelve, which the panel only reaches when four screens change together - so
  in practice this no longer refuses either. It is a stop against a runaway,
  not a budget; the budget is enforced by the step floor instead, which is the
  only place it can be enforced without snapping a screen.

  What it was, and why that was wrong: one cell step costs 7.1 ms and
  drumStep() advances exactly one cell per frame period, so N cells share the
  period and a turn that took ~96 ms alone took ~96*N with N in flight. The cap
  held N to three to stop the wheels lagging the value they chase. But a screen
  over the cap does not slow down - it is refused outright and snaps - and once
  every screen is animated, a fourth cell moving somewhere on the panel is the
  common case rather than the rare one. That is the "some digits roll, some
  click" this replaces.

  All 28 cells on this panel could be in flight in principle. Nothing sends
  eight screens at once, and if it did the floor below would simply turn every
  wheel in two steps.
*/
#define ANIM_CELLS_IN_FLIGHT 12

/*
  Marker written into _shadow[] for a cell whose content is no longer known -
  an aborted slide left half a glyph there. Chosen because it can never equal
  a digit or the '-' padLeftRanged() produces, so renderCells() is guaranteed
  to clear and redraw exactly that cell.
*/
static const char CELL_UNKNOWN = (char)0xFF;

/*
  Scratch column buffer for fastDrawDigit(). File-scope rather than a class
  member: it only ever holds one glyph's worth of columns while that
  function runs, so there is nothing to gain from giving every instance its
  own copy, and keeping it off the class shrinks OledMonitorPanel itself.
  32 covers the widest glyph either DSEG7 face at 18pt/16pt uses (22 and 19
  px respectively) with headroom; fastDrawDigit() guards against anything
  wider instead of trusting that.
*/
static uint8_t colbuf[32];

/*
  Per-screen digit-cell geometry for renderCells()/commitCells(). One entry
  per SCR_* index, in PROGMEM since it is read only a handful of times per
  update and RAM is the scarce resource here. Numbers are derived from the
  DSEG7 GFXglyph tables (xAdvance/xOffset/width/yOffset) and the cursor X
  already used by each updateDisplayXxx() call site - see the class header
  comment on renderCells() for the geometry this encodes.

  Constraint that is easy to break by accident: a cell rectangle covers whole
  8-row pages, so on an 18pt screen it starts at row 16 and on the 16pt ALT
  screen at row 24. No label may ink a row at or below that, or redrawing one
  cell will erase part of the label - which a full repaint would then put back,
  so it shows up as a label that decays while a value changes and heals when
  the mode does. Verified margins: 18pt screens +2 rows, RADIO ALT +0 (its
  label ends exactly on row 15), ALT +8.
*/
struct CellGeom {
    uint8_t channel;   // TCA9548A_CHANNEL_* value
    uint8_t x;         // cursor x of digit cell 0
    uint8_t advance;   // cell pitch = the font's xAdvance
    uint8_t blitW;     // width of the pushed rectangle
    uint8_t page0;
    uint8_t pages;
    uint8_t digits;    // number of digit cells
    uint8_t fontIdx;   // 0 = DSEG7 18pt, 1 = DSEG7 16pt
};

/*
  The one digit field on this board, measured rather than guessed.

  DSEG7 20pt advances 32 per digit and inks 24 columns starting 4 past the
  cursor, so a cell at cursor X inks X+4..X+27. blitX is cursor+2 (see
  renderCells), so blitW must be 26 to cover it - exactly, with nothing to
  spare on the right.

  That leaves columns 60..65 free between cell 1 (blit ends at 59) and cell 2
  (blit starts at 66). The inHg decimal point lives there: see
  updateDisplayBaro(), which is why a cell redraw can never erase it.

  Rows: baseline 62 puts the tallest glyph on rows 24..62, which is pages
  3..7 - see DIGIT_BASELINE_Y in the header for why that is not 60.
*/
static const CellGeom cellGeomTable[] PROGMEM = {
    // SCR_BARO
    { TCA9548A_CHANNEL_BARO, 0, 32, 26, 3, 5, 4, 0 },
};


// Catches a SCR_* enum edit that forgets to update the table above, rather
// than letting renderCells() silently read past the end of it.
static_assert(sizeof(cellGeomTable) / sizeof(cellGeomTable[0]) == SCR_COUNT,
              "cellGeomTable must have exactly SCR_COUNT entries");

/*
  Maps a CellGeom::fontIdx to the actual font pointer. Kept out of the
  PROGMEM struct itself - a GFXfont* read back via memcpy_P would still be a
  flash address, so storing it there buys nothing and just duplicates what
  this switch already expresses.
*/
static const GFXfont *fontForIndex(uint8_t fontIdx)
{
    (void)fontIdx; // one screen, one face - kept as a hook, not a switch
    return &DSEG7Classic_Regular20pt7b;
}

OledMonitorPanel::OledMonitorPanel()
{
    _initialised = false;
    _currentChannel = 0xFF;
    _busFaultReported = false;
    _busTimeoutReported = false;
    _dirty = 0;
    _animMask = 0;
    _animFrames = ANIM_FRAMES_DEFAULT;
    _drumActive = 0;
    _drumCursor = 0;
    _drumUp     = 0;
    memset(_drumMoving, 0, sizeof(_drumMoving));
    // Nothing has been drawn yet, so no shadow describes any screen. The
    // device pool this object is placement-new'd into happens to be zeroed,
    // but renderCells() correctness should not rest on that.
    memset(_shadowSig, 0, sizeof(_shadowSig));
}

/*
  Frees the I2C bus if a slave is still holding SDA down, and reports whether
  it had to. Must run before Wire.begin(), which takes the two pins over.

  Why this is needed at all: SDA is driven by whichever device is talking, and
  a slave that was interrupted part-way through sending a byte keeps holding it
  low, waiting for clocks that will never come. The master cannot issue a START
  over that, so every later transaction fails - and on AVR, Wire busy-waits, so
  "fails" means the firmware never returns. The board goes silent before it
  answers the connector, which is what MobiFlight shows as a nameless module.

  Resetting the Mega mid-transaction is exactly how a slave ends up there, and
  that is precisely what flashing does: the OLEDs keep their power across an
  upload while the Mega restarts underneath them. Hence a fault that appears
  after some uploads and not others, and that a reset cannot clear - only
  cutting power to the panel could, until now.

  The cure is the standard one: pulse SCL by hand until the slave has clocked
  out the rest of its byte and lets SDA go, then fabricate a STOP so it returns
  to idle. Nine pulses is one byte plus the ACK, which is the most any slave
  can be waiting for.
*/
bool OledMonitorPanel::recoverI2CBus(void)
{
    pinMode(SCL, INPUT_PULLUP);
    pinMode(SDA, INPUT_PULLUP);
    delayMicroseconds(10);

    if (digitalRead(SDA) == HIGH) return false; // bus already idle

    for (uint8_t i = 0; i < 9 && digitalRead(SDA) == LOW; i++) {
        pinMode(SCL, OUTPUT);          // drive low; the pull-up raises it again
        digitalWrite(SCL, LOW);
        delayMicroseconds(5);
        pinMode(SCL, INPUT_PULLUP);
        delayMicroseconds(5);
    }

    // STOP is SDA rising while SCL is high - the one edge that means "idle".
    pinMode(SDA, OUTPUT);
    digitalWrite(SDA, LOW);
    delayMicroseconds(5);
    pinMode(SDA, INPUT_PULLUP);
    delayMicroseconds(5);

    return true;
}

void OledMonitorPanel::attach(uint8_t addrI2C, uint8_t animMask, uint8_t frames)
{
    _addrI2C = addrI2C;
    _currentChannel = 0xFF;
    _animMask   = animMask;
    // Clamped here rather than trusted: the value comes from a free-text
    // Config string, and 0 would divide by zero in drumStep().
    _animFrames = frames < ANIM_FRAMES_MIN ? ANIM_FRAMES_MIN
                : frames > ANIM_FRAMES_MAX ? ANIM_FRAMES_MAX
                                           : frames;
    _drumActive = 0;
    _busFaultReported = false;
    _busTimeoutReported = false;

    bool recovered = recoverI2CBus();

    Wire.begin();
    Wire.setClock(400000);

    /*
      The second half of the fix, and the more important half: a stuck bus must
      never again be able to stop the firmware talking.

      Without this, twi_writeTo() spins on a status register with no way out,
      so any bus fault - a loose SDA, an unpowered panel, a half-finished byte
      this recovery could not clear - takes the whole board down silently. With
      it, the transaction gives up, the TWI hardware is reset, and the board
      goes on answering the connector, which is the difference between a panel
      that is not drawing and a board that has disappeared.

      25 ms is the library default and is enormous next to any real transaction
      here: Adafruit sends the framebuffer in 32-byte chunks, ~0.8 ms each at
      400 kHz. It cannot fire on healthy traffic.
    */
    Wire.setWireTimeout(25000, true);

    if (recovered)
        cmdMessenger.sendCmd(kStatus, F("Custom Device: I2C bus was stuck - recovered"));
    if (!FitInMemory(sizeof(OLEDInterface))) {
        // Error Message to Connector
        cmdMessenger.sendCmd(kStatus, F("Custom Device does not fit in Memory"));
        return;
    }
    if (_addrI2C & 0x01) {
        oled = new (allocateMemory(sizeof(OLEDInterface))) OLEDInterface(SSD1306);
    } else {
        oled = new (allocateMemory(sizeof(OLEDInterface))) OLEDInterface(SH1106);
    }
    _initialised = true;
}

void OledMonitorPanel::begin()
{
    if (!_initialised)
        return;

    setTCAChannel(TCA9548A_CHANNEL_BARO);
    oled->begin(SCREEN_ADDRESS, true); // 0x3C - the display's own address, not the mux's
    // Without this a glyph that would cross the right edge is moved to the
    // start of the next line instead of being clipped.
    oled->setTextWrap(false);
    oled->display();
    updateDisplayBaro();
}

void OledMonitorPanel::detach()
{
    if (!_initialised)
        return;
    _initialised = false;
    _drumActive  = 0;
}

void OledMonitorPanel::set(int16_t messageID, char *message)
{
    /* **********************************************************************************
        Each messageID has it's own value
        check for the messageID and define what to do.
        Important Remark!
        MessageID == -1 will be send from the connector when Mobiflight is closed
        Put in your code to shut down your custom device (e.g. clear a display)
        MessageID == -2 will be send from the connector when PowerSavingMode is entered
        Put in your code to enter this mode (e.g. clear a display)

    ********************************************************************************** */
    // do something according your messageID
    if (!_initialised)
        return;

    switch (messageID) {
    case 0: {
        // Efis Left Baro Select: 0 = inHg, 1 = hPa. Changes which value is
        // shown AND whether the decimal point is drawn, so it is a layout
        // change, not just a value - see the signature in updateDisplayBaro().
        uint8_t v = atoi(message);
        if (v != baroSelect) {
            baroSelect = v;
            _dirty |= (1 << SCR_BARO);
        }
        break;
    }

    case 1:
        // Efis Left Baro Value Hpa
        if (updateValue(baroValueHpa, sizeof(baroValueHpa), message))
            _dirty |= (1 << SCR_BARO);
        break;

    case 2:
        // Efis Left Baro Value Hg, sent without its decimal point
        if (updateValue(baroValueHg, sizeof(baroValueHg), message))
            _dirty |= (1 << SCR_BARO);
        break;

    case 3: {
        // Efis Left Baro Mode: 0 = QFE, 1 = QNH, 2 and 3 = STD
        uint8_t v = atoi(message);
        if (v != baroMode) {
            baroMode = v;
            _dirty |= (1 << SCR_BARO);
        }
        break;
    }

    case 19: {
        // Light Test
        uint8_t v = atoi(message);
        if (v != lightTestOn) {
            lightTestOn = v;
            _dirty |= (1 << SCR_BARO);
        }
        break;
    }

    case -1:
    case -2:
        blankAllDisplays();
        _dirty = 0; // don't let a pending redraw re-light the screen after shutdown
        break;

    default:
        // Ids 4..18 and 20 belong to the seven screens this board does not
        // have. The connector may still send them if the profile drives a
        // full panel; ignoring them costs nothing and keeps one profile
        // usable for both boards.
        break;
    }
}

void OledMonitorPanel::update()
{
    if (!_initialised)
        return;

    // Say so if the bus timed out. Without this the recovery is worse than the
    // hang in one respect: the board keeps answering the connector while the
    // panel quietly stops updating, and there is nothing to see. Reported once
    // - a genuinely broken bus times out on every transaction.
    if (!_busTimeoutReported && Wire.getWireTimeoutFlag()) {
        _busTimeoutReported = true;
        Wire.clearWireTimeoutFlag();
        cmdMessenger.sendCmd(kStatus, F("Custom Device: I2C timed out - display data may be lost"));
    }

    // A new value for a screen that is already turning needs no special case
    // here at all: it goes through renderScreen() like any other, and
    // slideCells() moves the wheel's target instead of starting over. That is
    // the difference between a wheel and the fixed-length slide this replaced,
    // which had to abort - and an aborted roll is what a snapped digit is.
    if (_drumActive) {
        // Frame clock, deliberately NOT MF_CUSTOMDEVICE_POLL_MS. That define
        // throttles the whole of update(), so it would also delay ordinary
        // repaints of the unanimated screens by up to a frame period and
        // stretch a Light Test burst of eight screens to eight frame periods
        // of wall clock. Gating only the slide leaves every other path exactly
        // as it was.
        //
        // Clocked from now rather than from _lastFrameMs + ANIM_FRAME_MS: if
        // update() was starved (a full repaint is 55 ms, more than four frame
        // periods) the catch-up form would fire several cells back to back,
        // which is precisely the burst this budget exists to prevent. Losing a
        // little smoothness beats losing the margin.
        if ((uint32_t)(millis() - _lastFrameMs) >= ANIM_FRAME_MS) {
            _lastFrameMs = millis();
            drumStep();
            return; // one cell is the whole budget for this call
        }
    }

    if (!_dirty)
        return;

    // Render at most one screen per call, so a burst of set() calls (e.g.
    // Light Test) spreads its ~25-30 ms-per-screen I2C cost across multiple
    // loop() iterations instead of blocking serial RX for ~240 ms straight.
    uint8_t scr = __builtin_ctz(_dirty); // lowest set bit
    _dirty &= ~((uint8_t)1 << scr);
    renderScreen(scr);
}

/*
  Dispatch a single screen index (SCR_*) to its updateDisplayXxx() renderer.
*/
void OledMonitorPanel::renderScreen(uint8_t scr)
{
    if (scr == SCR_BARO)
        updateDisplayBaro();
}

/* ************************************************************************************************
 ************************************************************************************************
 ************************************************************************************************ */

/*
  switch multiplexer channel
*/
void OledMonitorPanel::setTCAChannel(byte i)
{
    if (_currentChannel == i) return;
    Wire.beginTransmission(_addrI2C);
    Wire.write(1 << i);
    uint8_t err = Wire.endTransmission();

    // A multiplexer that does not answer used to be invisible: the firmware
    // went on drawing into a bus with nothing on it. Reported once rather than
    // per transaction, because a dead TCA9548A fails on every one of them and
    // would otherwise flood the connector faster than it could read.
    if (err != 0 && !_busFaultReported) {
        _busFaultReported = true;
        cmdMessenger.sendCmd(kStatus, F("Custom Device: multiplexer not responding - check I2C address and power"));
    }

    // The multiplexer needs to settle before the next transaction reaches the
    // panel behind it. Without this the bus occasionally locks up on the very
    // first switch at boot, and Wire on AVR busy-waits with no timeout, so the
    // board hangs before it ever answers the connector - it shows up in
    // MobiFlight as a nameless "Compatible" module with no serial.
    //
    // The original firmware used delay(5) here. That was removed as
    // copy-paste, which left the margin thin; holding the bus at 400 kHz for
    // our own transactions (see OLEDInterface) then cut it further, and from
    // there whether a given build survived depended on its exact instruction
    // timing. 100 us was measured to be enough, twice the value that already
    // worked, and costs 0.8 ms across all eight screens at boot - against
    // 55 ms for a single full repaint.
    delayMicroseconds(100);
    _currentChannel = i;
}

/*
  Blank all eight OLED displays (called on shutdown / power saving)
*/
void OledMonitorPanel::blankAllDisplays(void)
{
    if (!_initialised)
        return;

    setTCAChannel(TCA9548A_CHANNEL_BARO);
    oled->clearDisplay();
    oled->display();

    // Stop the wheel too. set() clears _dirty on shutdown, so nothing else
    // would, and the drum would happily keep turning digits onto a screen
    // that was just blanked.
    _drumActive = 0;
    memset(_drumMoving, 0, sizeof(_drumMoving));

    // The panel is blank, which no shadow describes.
    _shadowSig[SCR_BARO] = 0;
}

/*
  Rasterises glyph `c` of `font` straight into the shared page-major
  framebuffer, OR-ing bits in rather than going through Adafruit_GFX's
  drawPixel-per-pixel drawChar(). Measured 1246 us against 4589 us for one
  DSEG7 18pt glyph on the actual board. Restricted to the page range
  [page0, page0 + pages) - the caller is expected to pass the same range it
  intends to push with displayRegion(), so nothing outside that range is
  touched.

  Baseline is always row DIGIT_BASELINE_Y on this panel, so it is not a
  parameter - every digit cell on every screen shares one baseline.

  `yShift` moves the glyph down (positive) or up (negative) from that
  baseline, which is all the slide animation needs. Clipping to the cell is
  free and needs no extra work: the page loop below only ever runs over
  [page0, page0 + pages), and a glyph row that falls outside the shifted band
  simply fails the `gr` range test, so a glyph shifted a whole cell height
  writes nothing at all. Defaulted, so every existing call site is unchanged.
*/
void OledMonitorPanel::fastDrawDigit(uint8_t cursorX, uint8_t page0, uint8_t pages,
                                      const GFXfont *font, char c, int16_t yShift)
{
    GFXglyph *g  = &(((GFXglyph *)pgm_read_ptr(&font->glyph))[c - pgm_read_byte(&font->first)]);
    uint8_t  *bm = (uint8_t *)pgm_read_ptr(&font->bitmap);
    uint16_t  bo = pgm_read_word(&g->bitmapOffset);
    uint8_t   gw = pgm_read_byte(&g->width);
    uint8_t   gh = pgm_read_byte(&g->height);
    int8_t    xo = pgm_read_byte(&g->xOffset);
    int8_t    yo = pgm_read_byte(&g->yOffset);

    // A glyph wider than colbuf can never happen with the fonts this panel
    // uses, but guard it rather than trust that silently.
    if (gw > sizeof(colbuf)) return;

    // The framebuffer is the only thing between this and the heap. Every
    // caller passes geometry from cellGeomTable, which fits - but a stray
    // cursorX must not be able to write past the end of a page.
    int16_t left = (int16_t)cursorX + xo;
    if (left < 0 || left + gw > SCREEN_WIDTH) return;

    uint8_t *fb = oled->getBuffer();

    int16_t topRow = (int16_t)DIGIT_BASELINE_Y + yo - (int16_t)page0 * 8 + yShift; // glyph top, relative to page0

    for (uint8_t p = 0; p < pages; p++) {
        memset(colbuf, 0, gw);
        int16_t bandTop = (int16_t)p * 8;
        for (uint8_t b = 0; b < 8; b++) {
            int16_t gr = bandTop + b - topRow;       // row inside the glyph
            if (gr < 0 || gr >= (int16_t)gh) continue;
            uint16_t idx   = (uint16_t)gr * gw;
            uint16_t byteI = bo + (idx >> 3);
            uint8_t  bit   = 0x80 >> (idx & 7);
            uint8_t  bits  = pgm_read_byte(bm + byteI);
            uint8_t  mask  = 1 << b;
            for (uint8_t cx = 0; cx < gw; cx++) {
                if (bits & bit) colbuf[cx] |= mask;
                bit >>= 1;
                if (!bit) { bit = 0x80; bits = pgm_read_byte(bm + (++byteI)); }
            }
        }
        uint8_t *dst = fb + (uint16_t)(page0 + p) * SCREEN_WIDTH + cursorX + xo;
        for (uint8_t cx = 0; cx < gw; cx++) dst[cx] |= colbuf[cx];
    }
}

/*
  Zeroes a blitW x (pages * 8px) rectangle of the framebuffer at column
  blitX, pages [page0, page0 + pages). A plain memset per page measures
  43 us for 5 pages of 24 columns, against 389 us for the equivalent
  fillRect() - fillRect draws pixel by pixel and doesn't know the rectangle
  is page-aligned, so it can't do the memset Adafruit_GFX itself would
  reach for if it exposed one.
*/
void OledMonitorPanel::clearCell(uint8_t blitX, uint8_t page0, uint8_t pages, uint8_t blitW)
{
    uint8_t *fb = oled->getBuffer();
    for (uint8_t p = 0; p < pages; p++) {
        memset(fb + (uint16_t)(page0 + p) * SCREEN_WIDTH + blitX, 0, blitW);
    }
}

/*
  Redraws only the digit cells whose character changed since the last
  renderCells()/commitCells() on this screen, and pushes only those cells'
  rectangles over I2C - the fast path this whole file exists for. Returns
  false when the caller must fall back to a full repaint instead: the
  layout signature `sig` does not match what commitCells() last recorded
  for this screen (mode change, label change, different font - anything
  that moved something renderCells() does not know how to erase), or this
  screen has no cell geometry at all.

  `cells` must be a NUL-terminated string of exactly geom.digits characters,
  one per cell, left to right. `sig` must be non-zero - 0 is reserved to
  mean "unknown" in _shadowSig.
*/
bool OledMonitorPanel::renderCells(uint8_t scr, const char *cells, uint8_t sig)
{
    if (!_initialised) return false;
    if (scr >= SCR_COUNT) return false;

    CellGeom geom;
    memcpy_P(&geom, &cellGeomTable[scr], sizeof(CellGeom));

    if (geom.digits == 0) return false;        // this screen has no partial path
    if (sig == 0) return false;
    if (_shadowSig[scr] != sig) return false;   // layout on screen does not match - need a full repaint
    if (strlen(cells) != geom.digits) return false;

    const GFXfont *font = fontForIndex(geom.fontIdx);

    setTCAChannel(geom.channel);
    for (uint8_t i = 0; i < geom.digits; i++) {
        if (cells[i] == _shadow[scr][i]) continue; // unchanged cell - zero cost

        uint8_t cursorX = geom.x + i * geom.advance;
        uint8_t blitX   = cursorX + 2;

        clearCell(blitX, geom.page0, geom.pages, geom.blitW);
        fastDrawDigit(cursorX, geom.page0, geom.pages, font, cells[i]);
        oled->displayRegion(blitX, geom.page0, geom.blitW, geom.pages);

        _shadow[scr][i] = cells[i];
    }
    return true;
}

/*
  Records what a full repaint just put on `scr`, so the next update for
  that screen can go through renderCells() instead of a full repaint. Call
  this at the end of a full repaint (i.e. an updateDisplayXxx() body), with
  the same `cells`/`sig` a following renderCells() call would use.
*/
void OledMonitorPanel::commitCells(uint8_t scr, const char *cells, uint8_t sig)
{
    if (!_initialised) return;
    if (scr >= SCR_COUNT) return;

    uint8_t len = strlen(cells);
    if (len > sizeof(_shadow[0]) - 1) len = sizeof(_shadow[0]) - 1;
    memcpy(_shadow[scr], cells, len);
    _shadow[scr][len] = '\0';
    _shadowSig[scr] = sig;
}

/*
  Advances one digit wheel by one step and redraws it - the whole budget for
  this call, and the reason several cells can turn at once.

  Motion is a proportional approach: each step closes DRUM_DECEL_SHIFT worth of
  the remaining distance, with a floor so the tail cannot crawl. That gives a
  quick departure and a soft landing without an easing table, and - the part
  that matters - it has no notion of "frame 3 of 8", so a wheel whose target
  moves mid-turn just has further to travel. Nothing is ever restarted.

  Wheels are taken in turn from _drumCursor, so a screen the sim drives hard
  cannot starve the others.

  Rendering is two glyphs straight into the framebuffer, no canvas: the digit
  the wheel is leaving and the one after it, one cell height apart. Which pair
  that is falls out of the position, so it works the same turning up or down.
  Clipping is free - fastDrawDigit() only ever writes pages [page0, page0 +
  pages), so the parts that have rotated out of the cell cost nothing to hide.
*/
/*
  Digit cells turning right now, across every screen. Both the step floor and
  the panel-wide budget need this number, and it costs eight popcounts - far
  below the 7.1 ms frame it helps size, so it is recomputed rather than kept in
  a counter that every start, settle and abort would have to hold honest.
*/
uint8_t OledMonitorPanel::cellsInFlight(void) const
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < SCR_COUNT; i++) {
        uint8_t m = _drumMoving[i];
        while (m) { n++; m &= (uint8_t)(m - 1); }
    }
    return n;
}

void OledMonitorPanel::drumStep(void)
{
    if (!_drumActive) return;

    // Next screen with a wheel still turning, starting after the last served.
    uint8_t scr = SCR_COUNT;
    for (uint8_t n = 0; n < SCR_COUNT; n++) {
        uint8_t i = (uint8_t)((_drumCursor + 1 + n) % SCR_COUNT);
        if (_drumMoving[i]) { scr = i; break; }
    }
    if (scr == SCR_COUNT) { _drumActive = 0; return; }
    _drumCursor = scr;

    uint8_t cell = __builtin_ctz(_drumMoving[scr]);

    CellGeom geom;
    memcpy_P(&geom, &cellGeomTable[scr], sizeof(CellGeom));
    const GFXfont *font = fontForIndex(geom.fontIdx);

    bool     up     = (_drumUp & ((uint8_t)1 << scr)) != 0;
    uint16_t pos    = _drumPos[scr][cell];
    uint16_t target = (uint16_t)_drumTarget[scr][cell] * 256;

    // Distance still to travel in the direction of turn, wrapping through 9-0.
    uint16_t dist = up ? (uint16_t)((target - pos + DRUM_SPAN) % DRUM_SPAN)
                       : (uint16_t)((pos - target + DRUM_SPAN) % DRUM_SPAN);

    /*
      The step floor - and the one place the panel-wide cost is actually paid.

      _animFrames keeps its documented meaning, more frames meaning a slower
      wheel, by setting this floor rather than a frame count: 8 gives 24,
      2 gives 96.

      Scaling it by the number of cells in flight is what lets every cell that
      wants to turn actually turn. drumStep() advances one cell per frame
      period, so N cells share the period; without this, N cells settle N times
      slower and the only defence against that was refusing to animate them at
      all. Coarsening each step by the same N holds the settle time roughly
      flat instead - 96 ms for one cell, 132 for two, 156 for three, 180 for
      five, 348 even for a fabricated twelve, against 96*N before - and the
      wheel still passes through intermediate positions, never fewer than two,
      which is the whole difference between a roll and a click.

      DRUM_MAX_STEP caps it at half a digit per step, so past eight cells in
      flight the floor stops growing and turns lengthen again, gently.
    */
    uint16_t floorStep = (uint16_t)(192 / _animFrames) * cellsInFlight();
    if (floorStep > DRUM_MAX_STEP) floorStep = DRUM_MAX_STEP;

    uint16_t step = dist >> DRUM_DECEL_SHIFT;
    if (step < floorStep)     step = floorStep;
    if (step > DRUM_MAX_STEP) step = DRUM_MAX_STEP;

    bool settling = (dist == 0 || step >= dist);
    if (settling) {
        pos = target;
    } else {
        pos = up ? (uint16_t)((pos + step) % DRUM_SPAN)
                 : (uint16_t)((pos + DRUM_SPAN - step) % DRUM_SPAN);
    }
    _drumPos[scr][cell] = pos;

    uint8_t h    = geom.pages * 8;
    uint8_t d0   = (uint8_t)(pos >> 8);              // digit the wheel is leaving
    uint8_t frac = (uint8_t)(pos & 0x00FF);
    uint8_t off  = (uint8_t)(((uint16_t)frac * h) >> 8);

    uint8_t cursorX = geom.x + cell * geom.advance;
    uint8_t blitX   = cursorX + 2;

    setTCAChannel(geom.channel);
    /*
      Which way the drum face travels, which is not the same question as which
      way the number is going.

      On a real instrument the wheel is read from the front, so counting up
      brings the next digit DOWN into view from above and pushes the current
      one out of the bottom. This drew it the other way round - the outgoing
      digit left through the top - which reads as counting down while the
      number counts up.

      So the digit the wheel is leaving moves with +off, and its successor
      enters a full cell height above it. Counting down mirrors on its own:
      there the wheel runs backwards through the same positions, so the
      outgoing digit leaves through the top, which is again what the drum does.
    */
    clearCell(blitX, geom.page0, geom.pages, geom.blitW);
    fastDrawDigit(cursorX, geom.page0, geom.pages, font, (char)('0' + d0), (int16_t)off);
    // At frac == 0 the follower sits a whole cell height away and would write
    // nothing, so skip its ~1.2 ms rather than rasterise it into the clip test.
    if (frac)
        fastDrawDigit(cursorX, geom.page0, geom.pages, font,
                      (char)('0' + (d0 + 1) % DRUM_DIGITS), (int16_t)off - (int16_t)h);
    oled->displayRegion(blitX, geom.page0, geom.blitW, geom.pages);

    if (settling) {
        // The cell now holds exactly one digit again, so the shadow describes
        // the panel and the next value for this screen can take the ordinary
        // partial path.
        _shadow[scr][cell] = (char)('0' + _drumTarget[scr][cell]);
        _drumMoving[scr] &= ~((uint8_t)1 << cell);
        if (_drumMoving[scr] == 0) _drumActive &= ~((uint8_t)1 << scr);
    }
}

/*
  Stops every wheel on a screen without letting it arrive, leaving the shadow
  honest about what that did to the panel. Only the refusal paths in
  slideCells() use this - an ordinary new value retargets instead.
*/
void OledMonitorPanel::abortDrum(uint8_t scr)
{
    if (scr >= SCR_COUNT) return;
    if (!(_drumActive & ((uint8_t)1 << scr))) return;

    // Mid-turn those cells hold halves of two glyphs, which no single
    // character describes - so poison them rather than pretend they still hold
    // the old digit. The alternative, dropping _shadowSig, would force a 55 ms
    // full repaint; this costs one ordinary cell redraw each, because
    // CELL_UNKNOWN can never compare equal to the incoming character.
    for (uint8_t i = 0; i < MAX_DIGIT_CELLS; i++) {
        if (_drumMoving[scr] & ((uint8_t)1 << i)) _shadow[scr][i] = CELL_UNKNOWN;
    }
    _drumMoving[scr] = 0;
    _drumActive &= ~((uint8_t)1 << scr);
}

/*
  Points this screen's digit wheels at `cells`, starting them if they are at
  rest and simply re-aiming them if they are already turning. Returns true when
  it took the screen over - the caller must then do nothing else this update.
  Returns false for everything else, which drops the caller back onto the
  ordinary renderCells() or full-repaint path with nothing left half-done.

  Call it immediately before renderCells(), with the same cells/sig.
*/
bool OledMonitorPanel::slideCells(uint8_t scr, const char *cells, uint8_t sig)
{
    if (!(_animMask & (uint8_t)(1 << scr))) return false; // opt-in, and off by default
    if (!_initialised || scr >= SCR_COUNT) return false;

    CellGeom geom;
    memcpy_P(&geom, &cellGeomTable[scr], sizeof(CellGeom));

    bool refuse = false;

    // Everything renderCells() would refuse on has to be refused here too: a
    // wheel starts from _shadow, so it is only meaningful when the shadow is
    // known to describe the panel.
    if (lightTestOn == 1) refuse = true;
    else if (geom.digits == 0 || geom.digits > MAX_DIGIT_CELLS) refuse = true;
    else if (sig == 0 || _shadowSig[scr] != sig) refuse = true;
    else if (strlen(cells) != geom.digits) refuse = true;

    if (!refuse) {
        for (uint8_t i = 0; i < geom.digits; i++) {
            // The dash fields padLeftRanged() produces (RADIO ALT above 2500
            // ft, VOR DME with no station tuned) are not positions on a wheel,
            // so there is nothing to turn to.
            if (cells[i] < '0' || cells[i] > '9') { refuse = true; break; }
            // A cell at rest has to start from a digit; one already turning
            // carries its own position and needs no shadow.
            if (!(_drumMoving[scr] & ((uint8_t)1 << i))
                && (_shadow[scr][i] < '0' || _shadow[scr][i] > '9')) { refuse = true; break; }
        }
    }

    if (refuse) {
        // Hand the screen back intact: a wheel left turning would fight the
        // repaint that is about to happen.
        abortDrum(scr);
        return false;
    }

    // Where the screen is logically headed right now - the target for a cell
    // that is turning, the settled digit for one that is not. Direction is
    // taken from this rather than from _shadow alone, so re-aiming a wheel
    // mid-turn follows the new change and not the one it started on.
    char cur[MAX_DIGIT_CELLS + 1];
    for (uint8_t i = 0; i < geom.digits; i++) {
        cur[i] = (_drumMoving[scr] & ((uint8_t)1 << i))
                     ? (char)('0' + _drumTarget[scr][i])
                     : _shadow[scr][i];
    }
    cur[geom.digits] = 0x00;

    int8_t cmp = (int8_t)memcmp(cells, cur, geom.digits);
    if (cmp == 0) return false; // nothing to aim at - let renderCells() no-op

    // Cells that would have to start from rest, and the panel-wide budget they
    // have to fit into. Cells already turning cost nothing extra: re-aiming a
    // wheel is free, it is the turning itself that spends the frame period.
    uint8_t starting = 0, alreadyTurning = 0;
    for (uint8_t i = 0; i < geom.digits; i++) {
        if (_drumMoving[scr] & ((uint8_t)1 << i)) alreadyTurning++;
        else if (cells[i] != _shadow[scr][i]) starting++;
    }

    if (starting) {
        if (starting + alreadyTurning > ANIM_MAX_CELLS) { abortDrum(scr); return false; }

        if (cellsInFlight() + starting > ANIM_CELLS_IN_FLIGHT) { abortDrum(scr); return false; }
    }

    // Direction is taken from the value as a whole, never per cell: across a
    // carry (350 -> 349) the units rise while the tens fall, and turning the
    // two opposite ways at once looks like a fault rather than a counter. Both
    // strings are the same length and all digits by now, so the byte compare
    // IS the numeric compare - no strtol, no overflow to think about.
    if (cmp > 0) _drumUp |= ((uint8_t)1 << scr);
    else         _drumUp &= ~((uint8_t)1 << scr);

    for (uint8_t i = 0; i < geom.digits; i++) {
        uint8_t to = (uint8_t)(cells[i] - '0');
        if (_drumMoving[scr] & ((uint8_t)1 << i)) {
            _drumTarget[scr][i] = to; // already turning - just re-aim it
        } else if (cells[i] != _shadow[scr][i]) {
            _drumPos[scr][i]    = (uint16_t)(_shadow[scr][i] - '0') * 256;
            _drumTarget[scr][i] = to;
            _drumMoving[scr] |= ((uint8_t)1 << i);
        }
    }
    _drumActive |= ((uint8_t)1 << scr);

    // First step now, not up to a frame period from now: the value has already
    // changed and the screen should start moving on the same update() an
    // unanimated screen would have snapped on.
    _lastFrameMs = millis();
    drumStep();
    return true;
}


/*******************************************
Has to be redone, only tests

******************************************/

/*
  Shared renderer for the "small label + big DSEG7 value, optional managed
  dot" screen shape (EFIS left/right, FCU SPD/HDG/ALT/FPA, AUX). Does the
  channel switch, clear, label draw, value draw, optional dot, and
  display() - callers only compute which text/cursor/font/dot to pass in
  (including any per-mode label-cursor swap or digit mutation, which stays
  in the caller). FCU VS is the one screen left as a hand-written function:
  its sign handling, dual fonts, and V/S-vs-FPA branching don't reduce to
  this shape without obscuring the logic.
*/
/*
  Draws text horizontally centred on the 128 px wide screen at baseline y,
  using whatever font is currently selected. The label positions in the
  original firmware were hand-tuned for three-character labels (x = 50), so
  anything longer drifted right - "VOR DME" is 63 px wide and ended up hard
  against the right edge, and "RADIO ALT" sat well left of centre. Measuring
  the string means a label can be renamed without re-tuning a magic x.

  Deliberately NOT used for the DSEG7 values. That font is monospaced but
  its ink is not: '1' is 3 px wide and sits at the right of its 29 px cell,
  while '0' is 22 px wide. Measuring the ink would centre "100" 19 px away
  from where it centres "000", so the digits would jump sideways as the
  value changed. Value x stays a per-screen constant, derived once from
  cell width x digit count.
*/
void OledMonitorPanel::printCentered(const char *text, int16_t y)
{
    int16_t  x1, y1;
    uint16_t w, h;

    oled->getTextBounds(text, 0, y, &x1, &y1, &w, &h);
    int16_t x = ((int16_t)SCREEN_WIDTH - (int16_t)w) / 2 - x1;
    if (x < 0) x = 0;
    oled->setCursor(x, y);
    oled->println(text);
}

void OledMonitorPanel::renderLabelValue(byte channel,
                                 const char *labelText, int16_t labelY, const GFXfont *labelFont,
                                 const char *valueText, int16_t valueX, int16_t valueY, const GFXfont *valueFont,
                                 bool drawDot, int16_t dotX, int16_t dotY)
{
    setTCAChannel(channel);
    oled->clearDisplay();
    oled->setTextColor(SSD1306_WHITE);
    oled->setTextSize(1);

    oled->setFont(labelFont);
    printCentered(labelText, labelY);

    oled->setFont(valueFont);
    oled->setCursor(valueX, valueY);
    oled->println(valueText);

    if (drawDot) {
        oled->fillCircle(dotX, dotY, 3, SSD1306_WHITE);
    }
    oled->display();
}

/*
  The one screen: the A320 EFIS left barometric reference.

  Three layouts share it, and only the last takes the partial path:

    Light test  - both labels and 8888, drawn whole.
    STD         - "5td" in a larger face across the middle, drawn whole.
    QFE / QNH   - one label plus four digits, which is a cell layout.

  The decimal point in inHg is drawn at x = 63, and that number is load
  bearing. fillCircle(63, y, 2) inks columns 61..65; cell 1's blit rectangle
  ends at column 59 and cell 2's begins at 66, so the dot sits in the gap and
  no cell redraw can touch it. It is one column clear on the left and one on
  the right - see cellGeomTable.
*/
void OledMonitorPanel::updateDisplayBaro(void)
{
    bool std = (baroMode == 2 || baroMode == 3);

    if (lightTestOn == 1) {
        setTCAChannel(TCA9548A_CHANNEL_BARO);
        oled->clearDisplay();
        oled->setTextColor(SSD1306_WHITE);
        oled->setTextSize(1);
        oled->setFont(&FreeSans9pt7b);
        oled->setCursor(0, 15);
        oled->print("QFE");
        oled->setCursor(85, 15);
        oled->print("QNH");
        oled->setFont(&DSEG7Classic_Regular20pt7b);
        oled->setCursor(0, DIGIT_BASELINE_Y);
        oled->print("8888");
        oled->fillCircle(63, 60, 2, SSD1306_WHITE);
        oled->display();
        // The panel now shows the test pattern, which no shadow describes.
        // Leaving the old entry would be worse than useless: when light test
        // goes off the value is usually unchanged, so every cell would compare
        // equal, nothing would be drawn, and the 8888 would stay up until the
        // value next moved.
        _shadowSig[SCR_BARO] = 0;
        return;
    }

    if (std) {
        setTCAChannel(TCA9548A_CHANNEL_BARO);
        oled->clearDisplay();
        oled->setTextColor(SSD1306_WHITE);
        oled->setTextSize(1);
        oled->setFont(&DSEG7Classic_Regular22pt7b);
        oled->setCursor(10, 60);
        oled->print("5td"); // a seven-segment "Std" - the face has no S or t
        oled->display();
        _shadowSig[SCR_BARO] = 0; // not a cell layout at all
        return;
    }

    bool hpa = (baroSelect == 1);
    char cells[MAX_DIGIT_CELLS + 1];
    padLeft(cells, 4, hpa ? baroValueHpa : baroValueHg);

    /*
      Layout signature. Both bits change what is on the screen outside the
      digit cells, so a change in either has to force the full repaint below
      rather than a partial one:

        bit0 - the QFE/QNH label, which sits above the cells;
        bit1 - the inHg decimal point, which sits between them. renderCells()
               would never redraw it, so without this it would survive a
               switch to hPa and read as 10.13.
    */
    uint8_t sig = 0x80 | (baroMode == 0 ? 0x01 : 0x00) | (hpa ? 0x00 : 0x02);

    if (slideCells(SCR_BARO, cells, sig)) return;
    if (renderCells(SCR_BARO, cells, sig)) return;

    setTCAChannel(TCA9548A_CHANNEL_BARO);
    oled->clearDisplay();
    oled->setTextColor(SSD1306_WHITE);
    oled->setTextSize(1);

    oled->setFont(&FreeSans9pt7b);
    if (baroMode == 0) {
        oled->setCursor(0, 15);
        oled->print("QFE");
    } else {
        oled->setCursor(85, 15);
        oled->print("QNH");
    }

    oled->setFont(&DSEG7Classic_Regular20pt7b);
    oled->setCursor(0, DIGIT_BASELINE_Y);
    oled->print(cells);

    if (!hpa)
        oled->fillCircle(63, 60, 2, SSD1306_WHITE);

    oled->display();
    commitCells(SCR_BARO, cells, sig);
}
