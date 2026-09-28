//WiFi + NTP clock + Open-Meteo weather on Waveshare 2.9" e-paper
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include <time.h>
#include <GxEPD2_BW.h>
#include <Meteocons_24pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

// ---- WIFI ----
const char* ssid = "WiFi-2GHz";
const char* password = "kcug0exzb7";

//display setup - pins 
GxEPD2_BW<GxEPD2_290_BS, GxEPD2_290_BS::HEIGHT> display(GxEPD2_290_BS(/*CS=*/5, /*DC=*/17, /*RST=*/16, /*BUSY=*/4));

//EST - pulls raw UTC time from the interent once, applies these rules and maintains local time
const char* tzString = "EST5EDT,M3.2.0,M11.1.0";  

//Open-Meteo API
  //coordiantes set to Worcester,MA
  //requests current conditions and daily high/lows/rain
const char* weatherURL =
  "http://api.open-meteo.com/v1/forecast?latitude=42.2626&longitude=-71.8023"
  "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max"
  "&current=temperature_2m,relative_humidity_2m,apparent_temperature,wind_speed_10m,precipitation,weather_code"
  "&timezone=America%2FNew_York&wind_speed_unit=mph&temperature_unit=fahrenheit&forecast_days=1";

//timing intervals
const unsigned long WEATHER_INTERVAL = 15UL * 60UL * 1000UL;  //every 15 min
const unsigned long RETRY_INTERVAL   = 60UL * 1000UL;         //retry after 1 min on failure
//e-ink ghosting - full refresh every N redraws to clear ghosting
const int FULL_REFRESH_EVERY = 30;  

//weather data struct 
struct Weather {
  double temp, feelsLike, high, low;
  int code, rain;
  bool valid;
} weather = {0, 0, 0, 0, 0, 0, false};

//track time and screen refresh cycle
unsigned long nextFetchAt = 0;
int lastMinute = -1;
int redrawCount = 0;

//weather code translator 
const char* weatherText(int code) {
  if (code == 0) return "Clear";
  if (code <= 3) return "Cloudy";
  if (code == 45 || code == 48) return "Fog";
  if (code >= 51 && code <= 57) return "Drizzle";
  if (code >= 61 && code <= 67) return "Rain";
  if (code >= 71 && code <= 77) return "Snow";
  if (code >= 80 && code <= 82) return "Showers";
  if (code >= 85 && code <= 86) return "Snow showers";
  if (code >= 95) return "Thunderstorm";
  return "Unknown";
}

//fetch API and JSON parsing 
bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return false;
  //initilaize unecrypted HTTP client to save ESP32 memory 
  WiFiClient client;
  HTTPClient http;
  http.begin(client, weatherURL);

  //send request
  int httpCode = http.GET();
  bool ok = false;
  if (httpCode == 200) {
    //parse the raw text into structred JSON
    JSONVar doc = JSON.parse(http.getString());
    if (JSON.typeof(doc) != "undefined") {
      weather.temp      = (double) doc["current"]["temperature_2m"];
      weather.feelsLike = (double) doc["current"]["apparent_temperature"];
      weather.code      = (int)    doc["current"]["weather_code"];
      weather.high      = (double) doc["daily"]["temperature_2m_max"][0];
      weather.low       = (double) doc["daily"]["temperature_2m_min"][0];
      weather.rain      = (int)    doc["daily"]["precipitation_probability_max"][0];
      weather.valid = true;
      ok = true;
    } 
    else {
      Serial.println("JSON parse failed");
    }
  } 
  else {
    Serial.printf("Weather request failed, code: %d\n", httpCode);
  }
  //free up mmeory and close connection
  http.end();
  return ok;
}

//Temperature with a hand-drawn degree symbol
void drawTemp(int x, int y, double value) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", (int)round(value));
  //print temperature value
  display.setFont(&FreeSansBold18pt7b);
  display.setCursor(x, y);
  display.print(buf);
  //measure how manh pixels wide the temp value is 
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(buf, x, y, &x1, &y1, &w, &h);
  //calcualte coordinates for degree sumbol
  int cx = x1 + w + 8;
  //draw 2 cocentric circles to make the degree ring
  display.drawCircle(cx, y - 30, 4, GxEPD_WHITE);
  display.drawCircle(cx, y - 30, 3, GxEPD_WHITE);  //thicker ring
  //F after the degree symbol
  display.setCursor(cx + 8, y);
  display.print("F");
}

//main graphics engine
void drawScreen(struct tm &t) {
  //24 hour format to 12 hour format
  int h12 = t.tm_hour % 12;
  if (h12 == 0) h12 = 12;

  char timeStr[8], dateStr[20];
  snprintf(timeStr, sizeof(timeStr), "%d:%02d", h12, t.tm_min);
  const char* ampm = (t.tm_hour >= 12) ? "PM" : "AM";
  strftime(dateStr, sizeof(dateStr), "%a, %b %d", &t);

  //manage ghosting - partial vs full update
  bool full = (redrawCount % FULL_REFRESH_EVERY == 0);
  redrawCount++;

  if (full) display.setFullWindow();
  else display.setPartialWindow(0, 0, display.width(), display.height());

  //drawing loop
  display.firstPage();
  do {
    //dark mode
    display.fillScreen(GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);

    //LEFT: time, date, condition, symbol
    display.setFont(&FreeSansBold18pt7b);
    display.setCursor(8, 45);
    display.print(timeStr);

    //measure time string width to see where AM/PM is placed
    int16_t x1, y1;
    uint16_t w, hgt;
    display.getTextBounds(timeStr, 8, 52, &x1, &y1, &w, &hgt);

    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(x1 + w + 6, 45);
    display.print(ampm);

    display.setFont(&FreeSans9pt7b);
    display.setCursor(8, 80);
    display.print(dateStr);

    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(8, 112);

    //weather condition text and symbol placement
    String conditionStr = weather.valid ? weatherText(weather.code) : "No data";
    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(8, 112);
    display.print(conditionStr);

    if (weather.valid) {
      int16_t cx, cy;
      uint16_t cw, ch;
      display.getTextBounds(conditionStr, 8, 112, &cx, &cy, &cw, &ch);
      drawWeatherIcon(weather.code, cx + cw + 10, 112); 
    }

    //DIVIDER
    display.drawLine(160, 0, 160, 128, GxEPD_WHITE);

    //RIGHT: temp, feels like, high/low, rain
    if (weather.valid) {
      drawTemp(170, 45, weather.temp);

      char buf[24];
      display.setFont(&FreeSans9pt7b);
      snprintf(buf, sizeof(buf), "Feels like  %dF", (int)round(weather.feelsLike));
      display.setCursor(170, 68);
      display.print(buf);

      display.setFont(&FreeSansBold9pt7b);
      snprintf(buf, sizeof(buf), "H %d  L %d", (int)round(weather.high), (int)round(weather.low));
      display.setCursor(170, 92);
      display.print(buf);

      display.setFont(&FreeSans9pt7b);
      snprintf(buf, sizeof(buf), "Rain %d%%", weather.rain);
      display.setCursor(170, 118);
      display.print(buf);
    } 
    else {
      display.setFont(&FreeSans9pt7b);
      display.setCursor(170, 60);
      display.print("Waiting for");
      display.setCursor(170, 80);
      display.print("weather...");
    }
  } 
  while (display.nextPage());
}

//WIFI connect
void connectWiFi() {
  WiFi.begin(ssid, password);
  Serial.print("Connecting");
  int attempts = 0;
  //block execution until connected or timeout is reached
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? "\nConnected!" : "\nWiFi failed, will keep retrying");
}

//weather symbol renderer using Meteocons font
void drawWeatherIcon(int code, int x, int y) {
  //switch to the symbol font
  display.setFont(&Meteocons_Regular_24); 
  display.setCursor(x, y);

  //map the weather code to the meteocons character
  if (code == 0) display.print("B");                               // Clear (Sun)
  else if (code <= 3) display.print("Y");                          // Cloudy
  else if (code == 45 || code == 48) display.print("M");           // Fog
  else if (code >= 51 && code <= 57) display.print("Q");           // Drizzle
  else if (code >= 61 && code <= 67) display.print("R");           // Rain
  else if (code >= 71 && code <= 77) display.print("W");           // Snow
  else if (code >= 80 && code <= 82) display.print("R");           // Showers (Rain)
  else if (code >= 85 && code <= 86) display.print("W");           // Snow showers
  else if (code >= 95) display.print("P");                         // Thunderstorm
  else display.print(")");                                         // Unknown
}

//SETUP
void setup() {
  Serial.begin(115200);
  //initialize e-ink display driver
  display.init(115200, true, 2, false);  //Waveshare-style reset timing
  display.setRotation(1);                //landscape: 296 x 128
  //connect to wifi
  connectWiFi();
  //sync the real time clock with NTP servers using EST rules
  configTzTime(tzString, "pool.ntp.org", "time.nist.gov");

  //since esp32 runs faster than network, this loop pauses the boot process for upto 10sec to ensure the exact time has downloaded before we move on
  struct tm t;
  int tries = 0;
  while (!getLocalTime(&t, 500) && tries < 20) {
    tries++;
  }
  //force API fetch so we have data when UI boots up
  fetchWeather();
  nextFetchAt = millis() + WEATHER_INTERVAL; //fetch every 15mins
}

//MAIN LOOP
void loop() {
  //if connection lost, reconnect in the background
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();  //clock on ESP32 keeps time on its own
  }

  //non blocking timer, comapres current time to the scheduled next fetch target
  if ((long)(millis() - nextFetchAt) >= 0) {
    bool ok = fetchWeather();
    //wait 15 mins on success or retry every 1 min on failure
    nextFetchAt = millis() + (ok ? WEATHER_INTERVAL : RETRY_INTERVAL);
    lastMinute = -1;  //force a redraw with the new data
  }

  //redraw only when the minute changes
  struct tm t;
  if (getLocalTime(&t, 100) && t.tm_min != lastMinute) {
    lastMinute = t.tm_min; //update tracker
    drawScreen(t); //render the new screen
  }

  delay(1000);
}