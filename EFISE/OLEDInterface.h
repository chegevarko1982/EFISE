#pragma once

#include <Arduino.h>
#include "allocateMem.h"
#include "commandmessenger.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_SH110X.h>

#define SCREEN_WIDTH   128  // OLED display width, in pixels
#define SCREEN_HEIGHT  64   // OLED display height, in pixels
#define OLED_RESET     -1   // Reset pin # (or -1 if sharing Arduino reset pin)
#define SCREEN_ADDRESS 0x3c // address of the displays. All displays uses the same address
#define I2C_CLOCK_HZ   400000UL // held for our own transactions too, see the constructor
#define SH1106_COLUMN_OFFSET 2  // Adafruit_SH1106G::_page_start_offset

enum OLEDType {
    SSD1306,
    SH1106
};

class OLEDInterface
{
public:
    OLEDInterface(OLEDType type)
    {
        _type = type;
        if (_type == SSD1306) {
            if (!FitInMemory(sizeof(Adafruit_SSD1306))) {
                // Error Message to Connector
                cmdMessenger.sendCmd(kStatus, F("Custom Device does not fit in Memory"));
                return;
            }
            // The last two arguments are clkDuring and clkAfter. clkAfter
            // defaults to 100 kHz, and the library RESTORES it after every
            // call it makes - so display() ran at 400 kHz while every
            // transaction this class issues itself (channel switch, region
            // push) ran at 100 kHz. Measured on the panel: that alone made
            // displayRegion() cost 14.4 ms instead of 4.5 ms.
            oled_1306 = new (allocateMemory(sizeof(Adafruit_SSD1306))) Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET, I2C_CLOCK_HZ, I2C_CLOCK_HZ);
        } else {
            if (!FitInMemory(sizeof(Adafruit_SH1106G))) {
                // Error Message to Connector
                cmdMessenger.sendCmd(kStatus, F("Custom Device does not fit in Memory"));
                return;
            }
            oled_1106 = new (allocateMemory(sizeof(Adafruit_SH1106G))) Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET, I2C_CLOCK_HZ, I2C_CLOCK_HZ);
        }
    }
    void begin(uint8_t I2Caddress, bool status)
    {
        if (_type == SSD1306)
            oled_1306->begin(SSD1306_SWITCHCAPVCC, I2Caddress, status);
        else
            oled_1106->begin(I2Caddress, status);
    }
    void display()
    {
        if (_type == SSD1306)
            oled_1306->display();
        else
            oled_1106->display();
    }
    void clearDisplay()
    {
        if (_type == SSD1306)
            oled_1306->clearDisplay();
        else
            oled_1106->clearDisplay();
    }
    void setTextColor(uint16_t color)
    {
        if (_type == SSD1306)
            oled_1306->setTextColor(color);
        else
            oled_1106->setTextColor(color);
    }
    void setFont(const GFXfont *f = NULL)
    {
        if (_type == SSD1306)
            oled_1306->setFont(f);
        else
            oled_1106->setFont(f);
    }
    void setCursor(int16_t x, int16_t y)
    {
        if (_type == SSD1306)
            oled_1306->setCursor(x, y);
        else
            oled_1106->setCursor(x, y);
    }
    void setTextWrap(bool w)
    {
        if (_type == SSD1306)
            oled_1306->setTextWrap(w);
        else
            oled_1106->setTextWrap(w);
    }
    // Measures the given string with the font/size currently set, so callers
    // can place it themselves (e.g. centre it). Must be called after setFont().
    void getTextBounds(const char *t, int16_t x, int16_t y, int16_t *x1, int16_t *y1, uint16_t *w, uint16_t *h)
    {
        if (_type == SSD1306)
            oled_1306->getTextBounds(t, x, y, x1, y1, w, h);
        else
            oled_1106->getTextBounds(t, x, y, x1, y1, w, h);
    }
    void setTextSize(uint8_t s)
    {
        if (_type == SSD1306)
            oled_1306->setTextSize(s);
        else
            oled_1106->setTextSize(s);
    }
    void fillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color)
    {
        if (_type == SSD1306)
            oled_1306->fillCircle(x0, y0, r, color);
        else
            oled_1106->fillCircle(x0, y0, r, color);
    }
    void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color)
    {
        if (_type == SSD1306)
            oled_1306->drawFastVLine(x, y, h, color);
        else
            oled_1106->drawFastVLine(x, y, h, color);
    }
    void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color)
    {
        if (_type == SSD1306)
            oled_1306->drawFastHLine(x, y, w, color);
        else
            oled_1106->drawFastHLine(x, y, w, color);
    }
    void println(const char *t)
    {
        if (_type == SSD1306)
            oled_1306->println(t);
        else
            oled_1106->println(t);
    }
    void println(String t)
    {
        if (_type == SSD1306)
            oled_1306->println(t);
        else
            oled_1106->println(t);
    }
    void print(const char *t)
    {
        if (_type == SSD1306)
            oled_1306->print(t);
        else
            oled_1106->print(t);
    }
    void print(String t)
    {
        if (_type == SSD1306)
            oled_1306->print(t);
        else
            oled_1106->print(t);
    }

    /* The raw framebuffer, one byte per column per page, 8 vertical pixels
       per byte. Shared by all eight panels behind the multiplexer. */
    uint8_t *getBuffer()
    {
        if (_type == SSD1306)
            return oled_1306->getBuffer();
        else
            return oled_1106->getBuffer();
    }

    /* Pushes only the columns [x, x+w) of the pages [page0, page0+pages).

       display() is all-or-nothing: 1024 bytes, 38 ms measured. One digit cell
       is 24 columns over 5 pages - 120 bytes, 4.5 ms. That difference is the
       whole point of the partial-update path.

       Neither library's display() can be trusted to stay inside a region, so
       both branches address the panel directly and walk the buffer here.
       SSD1306 has no dirty tracking at all. SH1106's display() does track a
       dirty rectangle but only clamps it horizontally, so it always pushes
       from the first dirty page down to the last page on the panel - which,
       with one buffer shared by eight screens, means pushing whatever the
       previously drawn screen left there.

       Callers must have rewritten the whole rectangle first: it is addressed
       in whole 8-row pages, so anything stale inside those pages goes out too. */
    void displayRegion(uint8_t x, uint8_t page0, uint8_t w, uint8_t pages)
    {
        if (x >= SCREEN_WIDTH || !w || !pages) return;
        if (x + w > SCREEN_WIDTH) w = SCREEN_WIDTH - x;
        if (page0 + pages > SCREEN_HEIGHT / 8) pages = SCREEN_HEIGHT / 8 - page0;

        uint8_t *buf = getBuffer();

        if (_type == SSD1306) {
            // One transaction for the whole command stream: control byte 0x00
            // means "commands follow", so these six do not need six
            // transactions. Transaction overhead is ~87 us at 400 kHz, which
            // is the dominant cost at this size.
            Wire.beginTransmission(SCREEN_ADDRESS);
            Wire.write((uint8_t)0x00);
            Wire.write((uint8_t)SSD1306_COLUMNADDR);
            Wire.write(x);
            Wire.write((uint8_t)(x + w - 1));
            Wire.write((uint8_t)SSD1306_PAGEADDR);
            Wire.write(page0);
            Wire.write((uint8_t)(page0 + pages - 1));
            Wire.endTransmission();

            // Horizontal addressing: the write pointer walks the window by
            // itself, so every page streams on without re-addressing.
            uint8_t pending = 0;
            for (uint8_t p = 0; p < pages; p++) {
                uint8_t *src = buf + (uint16_t)(page0 + p) * SCREEN_WIDTH + x;
                for (uint8_t i = 0; i < w; i++) {
                    if (pending == 0) {
                        Wire.beginTransmission(SCREEN_ADDRESS);
                        Wire.write((uint8_t)0x40); // data stream follows
                        pending = 30;              // Wire BUFFER_LENGTH 32, minus address and control byte
                    }
                    Wire.write(src[i]);
                    if (--pending == 0) Wire.endTransmission();
                }
            }
            if (pending != 0) Wire.endTransmission();
        } else {
            // SH1106 is page addressed: there is no window command, only
            // "select this page" plus a column start, reissued per page.
            for (uint8_t p = 0; p < pages; p++) {
                uint8_t col = (uint8_t)(x + SH1106_COLUMN_OFFSET);
                oled_1106->oled_command((uint8_t)(SH110X_SETPAGEADDR + page0 + p));
                oled_1106->oled_command((uint8_t)(0x10 + (col >> 4)));
                oled_1106->oled_command((uint8_t)(col & 0x0F));

                uint8_t *src     = buf + (uint16_t)(page0 + p) * SCREEN_WIDTH + x;
                uint8_t  pending = 0;
                for (uint8_t i = 0; i < w; i++) {
                    if (pending == 0) {
                        Wire.beginTransmission(SCREEN_ADDRESS);
                        Wire.write((uint8_t)0x40);
                        pending = 30;
                    }
                    Wire.write(src[i]);
                    if (--pending == 0) Wire.endTransmission();
                }
                if (pending != 0) Wire.endTransmission();
            }
        }
    }

private:
    OLEDType          _type;
    Adafruit_SSD1306 *oled_1306;
    Adafruit_SH1106G *oled_1106;
};
