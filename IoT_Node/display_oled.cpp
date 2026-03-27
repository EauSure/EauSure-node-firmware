#include "display_oled.h"
#include "app_state.h"

static void drawBar(int x, int y, int w, int h, int val, int maxVal, const String& label) {
  display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
  display.setCursor(x, y);
  display.print(label);

  int barX = x + 20;
  int barW = w - 20;

  display.drawRect(barX, y, barW, h, SSD1306_WHITE);

  int fillW = (val * (barW - 2)) / maxVal;
  fillW = constrain(fillW, 0, barW - 2);

  display.fillRect(barX + 1, y + 1, fillW, h - 2, SSD1306_WHITE);

  if (fillW < barW - 2) {
    display.fillRect(barX + 1 + fillW, y + 1, (barW - 2) - fillW, h - 2, SSD1306_BLACK);
  }
}

void displayInit() {
  display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.display();
}

void renderOLED() {
  display.clearDisplay();

  if (gEventState.isExploding) {
    uint32_t elapsed = millis() - gEventState.explosionStartAt;
    if (elapsed > 2000) {
      gEventState.isExploding = false;
    } else {
      int radius = elapsed / 40;
      display.drawCircle(64, 32, radius, SSD1306_WHITE);
      display.drawCircle(64, 32, radius + 5, SSD1306_WHITE);
      display.drawLine(64, 32, 64 + radius, 32 + radius, SSD1306_WHITE);
      display.drawLine(64, 32, 64 - radius, 32 - radius, SSD1306_WHITE);

      display.setTextSize(2);
      display.setCursor(25, 24);
      display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
      display.print(" SHAKE! ");
      display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
      display.display();
      return;
    }
  }

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);

  drawBar(0, 0, 90, 8, gSensorData.battPercent, 100, "B");
  display.setCursor(95, 0);
  display.printf("%3d%%", gSensorData.battPercent);

  drawBar(0, 11, 90, 8, gSensorData.tdsScale10, 10, "TD");
  display.setCursor(95, 11);
  display.printf("%2d/10", gSensorData.tdsScale10);

  drawBar(0, 22, 90, 8, gSensorData.turbScale10, 10, "TB");
  display.setCursor(95, 22);
  display.printf("%2d/10", gSensorData.turbScale10);

  drawBar(0, 33, 90, 8, gSensorData.phScale10, 10, "pH");
  display.setCursor(95, 33);
  display.printf("%2d/10", gSensorData.phScale10);

  display.setCursor(0, 44);
  display.printf("W:%.1fC  M:%.1fC  %.1fV", gSensorData.waterTempC, gSensorData.mpuTempC, gSensorData.battLoadV);

  display.setCursor(0, 55);
  display.print("Ev:");
  display.fillRect(20, 55, 108, 9, SSD1306_BLACK);
  display.setCursor(20, 55);
  display.print(gEventState.lastEvent);

  display.display();
}