#include "DisplayUI.h"
#include "config.h"

namespace {
// 128 px / 6 px per glyph = 21 characters per line at text size 1. Longer
// strings (a real student's full name) would wrap into the next line and
// overlap it, so clip instead.
const unsigned kMaxChars = OLED_SCREEN_WIDTH / 6;

String clip(const String& s) {
  return s.length() > kMaxChars ? s.substring(0, kMaxChars) : s;
}
}  // namespace

DisplayUI::DisplayUI()
    : _display(OLED_SCREEN_WIDTH, OLED_SCREEN_HEIGHT, &Wire, OLED_RESET_PIN) {}

bool DisplayUI::begin() {
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);

  // Adafruit_SSD1306::begin() only fails if it can't allocate its frame
  // buffer - it never checks that a display actually answered. So probe
  // the bus ourselves: a missing or miswired OLED is then reported as
  // such, and a module strapped to the other address just works.
  const uint8_t candidates[] = {OLED_I2C_ADDRESS, OLED_I2C_ADDRESS_ALT};
  _address = 0;
  for (uint8_t addr : candidates) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      _address = addr;
      break;
    }
  }
  if (_address == 0) {
    _ok = false;
    return false;
  }

  _ok = _display.begin(SSD1306_SWITCHCAPVCC, _address);
  if (_ok) _display.cp437(true);  // correct glyph mapping for extended characters
  return _ok;
}

void DisplayUI::showMessage(const String& title, const String& line1,
                             const String& line2, const String& line3) {
  // No display (failed begin()) -> the frame buffer was never allocated,
  // so drawing would crash. The system still works headless via LEDs,
  // buzzer and Serial.
  if (!_ok) return;

  _display.clearDisplay();
  _display.setTextColor(SSD1306_WHITE);
  _display.setTextSize(1);
  _display.setTextWrap(false);

  _display.setCursor(0, 0);
  _display.print(clip(title));
  _display.drawLine(0, 10, OLED_SCREEN_WIDTH, 10, SSD1306_WHITE);

  _display.setCursor(0, 16);
  _display.print(clip(line1));

  _display.setCursor(0, 32);
  _display.print(clip(line2));

  _display.setCursor(0, 48);
  _display.print(clip(line3));

  _display.display();
}

void DisplayUI::showReady(bool wifiConnected, int pendingUploads, bool readerOk) {
  String line3;
  if (!readerOk) {
    line3 = "RFID: check wiring!";
  } else if (pendingUploads > 0) {
    line3 = "Unsynced: " + String(pendingUploads);
  }
  showMessage("SYSTEM READY",
              wifiConnected ? "Wi-Fi: Connected" : "Wi-Fi: Offline",
              "Tap your ID Card", line3);
}
