#pragma once

namespace pins {

constexpr int i2c_sda = 1;
constexpr int i2c_scl = 2;

constexpr int tft_sck = 14;
constexpr int tft_mosi = 21;
constexpr int tft_miso = 47;
constexpr int tft_cs = 39;
constexpr int tft_dc = 40;
constexpr int touch_cs = 41;
constexpr int sd_cs = 46;

constexpr int rfid_sck = 20;
constexpr int rfid_mosi = 43;
constexpr int rfid_miso = 38;
constexpr int rfid_cs = 19;
constexpr int rfid_rst = 3;

constexpr int relay = 45;
constexpr int buzzer = 42;

namespace camera {
constexpr int pwdn = -1;
constexpr int reset = -1;
constexpr int xclk = 15;
constexpr int siod = 4;
constexpr int sioc = 5;
constexpr int y9 = 16;
constexpr int y8 = 17;
constexpr int y7 = 18;
constexpr int y6 = 12;
constexpr int y5 = 10;
constexpr int y4 = 8;
constexpr int y3 = 9;
constexpr int y2 = 11;
constexpr int vsync = 6;
constexpr int href = 7;
constexpr int pclk = 13;
}

}
