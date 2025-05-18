#include <WiFi.h>
#include <U8g2lib.h>
#include <HTTPClient.h>

#define WIFI_SSID     "po4ka"
#define WIFI_PASSWORD "zalupend"

const char* TWITCH_LINKS[3] = {
  "https://www.twitch.tv/alexeyxtype",
  "https://www.twitch.tv/assimaslow",
  "https://www.twitch.tv/loskych"
};

#define SDA_PIN 0
#define SCL_PIN 2

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE, /* clock=*/SCL_PIN, /* data=*/SDA_PIN);

#define CHANNELS_COUNT 3
#define CHECK_INTERVAL_OFFLINE 1800UL
#define CHECK_INTERVAL_ONLINE 600UL

struct Channel {
  String url;
  String name;
  String status;
  bool online;
  unsigned long nextCheck;
};

Channel channels[CHANNELS_COUNT];

String getChannelName(const String& url) {
  int idx = url.lastIndexOf('/');
  if (idx >= 0 && idx < (int)url.length() - 1)
    return url.substring(idx + 1);
  return url;
}

String trimToWidth(const String& text, uint8_t maxWidth, const uint8_t* font) {
  u8g2.setFont(font);
  String out = text;
  while (u8g2.getUTF8Width(out.c_str()) > maxWidth && out.length() > 2) {
    out.remove(out.length() - 1, 1);
  }
  return out;
}

String asciiOnly(const String& text) {
  String res = "";
  for (size_t i = 0; i < text.length(); i++) {
    char c = text[i];
    if ((c >= 32 && c <= 126) || c == '\n') res += c;
  }
  return res;
}

bool getTwitchStatusHTML(const String& channelURL, String& statusText) {
  HTTPClient http;
  http.setTimeout(10000);
  http.begin(channelURL);
  http.addHeader("User-Agent", "Mozilla/5.0 (compatible; ESP32C3/1.0)");
  int httpCode = http.GET();
  bool online = false;
  statusText = "net error";
  if (httpCode == 200) {
    String payload = http.getString();
    payload.toLowerCase();

    if (
      payload.indexOf("\"islivebroadcast\":true") != -1 ||
      payload.indexOf("\"islive\":true") != -1
    ) {
      online = true;
      statusText = "online";
    } else {
      online = false;
      statusText = "offline";
    }
  }
  http.end();
  delay(200);
  return online;
}

// Блок чуть выше, чтобы добавить пространство между статусом и линией
#define BLOCK_HEIGHT 26
#define LINE_OFFSET_FROM_STATUS 6  // px ниже статуса

#define SCROLL_SPEED 0.2f
#define SCROLL_DELAY_MS 60
#define SCROLL_PAUSE_MS 1000

unsigned long lastScroll = 0;
float scrollY = 0.0;
bool scrollDirDown = true;
bool scrollPause = false;
unsigned long pauseStart = 0;

void drawDisplay() {
  u8g2.clearBuffer();

  int maxNameWidth = 120;
  int maxStatusWidth = 120;
  int blocksPerScreen = 64 / BLOCK_HEIGHT + 2;

  for (int i = 0; i < blocksPerScreen; i++) {
    int idx = (int)(i + scrollY / BLOCK_HEIGHT);
    if (idx >= CHANNELS_COUNT) continue;
    int y = (i * BLOCK_HEIGHT) - ((int)scrollY % BLOCK_HEIGHT);

    String name = trimToWidth(channels[idx].name, maxNameWidth, u8g2_font_6x13_tr);
    u8g2.setFont(u8g2_font_6x13_tr);
    u8g2.drawStr(0, y + 12, name.c_str());

    String status = asciiOnly(channels[idx].status);
    status = trimToWidth(status, maxStatusWidth, u8g2_font_5x8_tr);
    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawStr(0, y + 21, status.c_str());

    // Разделительная линия ниже блока + расстояние
    if (i < blocksPerScreen - 1 && idx < CHANNELS_COUNT - 1) {
      u8g2.drawLine(0, y + 21 + LINE_OFFSET_FROM_STATUS, 127, y + 21 + LINE_OFFSET_FROM_STATUS);
    }
  }
  u8g2.sendBuffer();
}

unsigned long lastCheck = 0;
void checkChannels() {
  unsigned long now = millis() / 1000UL;
  for (int i = 0; i < CHANNELS_COUNT; i++) {
    if (channels[i].nextCheck > now) continue;

    String statusText;
    bool isOnline = getTwitchStatusHTML(channels[i].url, statusText);
    channels[i].online = isOnline;
    channels[i].status = statusText;

    if (channels[i].online) {
      channels[i].nextCheck = now + CHECK_INTERVAL_ONLINE;
    } else {
      channels[i].nextCheck = now + CHECK_INTERVAL_OFFLINE;
    }
    Serial.printf("Канал %s: %s (%s)\n", channels[i].name.c_str(), channels[i].online ? "ON AIR" : "OFFLINE", channels[i].status.c_str());
  }
}

void setup() {
  Serial.begin(115200);
  u8g2.begin();

  for (int i = 0; i < CHANNELS_COUNT; i++) {
    channels[i].url = String(TWITCH_LINKS[i]);
    channels[i].name = getChannelName(channels[i].url);
    channels[i].status = "...";
    channels[i].online = false;
    channels[i].nextCheck = 0;
  }

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(2, 24, "WiFi connect...");
  u8g2.sendBuffer();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - wifiStart > 30000) {
      u8g2.clearBuffer();
      u8g2.drawStr(2, 40, "WiFi FAIL!");
      u8g2.sendBuffer();
      delay(5000);
      ESP.restart();
    }
  }
}

void loop() {
  unsigned long now = millis();
  if (now - lastCheck > 10000) {
    checkChannels();
    lastCheck = now;
  }

  int maxScrollY = (CHANNELS_COUNT - (64 / BLOCK_HEIGHT)) * BLOCK_HEIGHT;
  if (maxScrollY < 0) maxScrollY = 0;
  bool atEnd = (scrollDirDown && scrollY >= maxScrollY);
  bool atStart = (!scrollDirDown && scrollY <= 0);

  if (scrollPause) {
    if (now - pauseStart > SCROLL_PAUSE_MS) {
      scrollPause = false;
      if (atEnd) scrollDirDown = false;
      if (atStart) scrollDirDown = true;
    }
  } else {
    if ((atEnd || atStart)) {
      scrollPause = true;
      pauseStart = now;
    } else if (now - lastScroll > SCROLL_DELAY_MS) {
      scrollY += (scrollDirDown ? SCROLL_SPEED : -SCROLL_SPEED);
      if (scrollY < 0) scrollY = 0;
      if (scrollY > maxScrollY) scrollY = maxScrollY;
      lastScroll = now;
    }
  }

  drawDisplay();
}