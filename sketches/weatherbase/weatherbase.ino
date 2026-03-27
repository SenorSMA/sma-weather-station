/* --------------------------------------------------------------------------------
 *  Weatherbase
 *  Harald Schlangmann, March 2021
 *
 *  MODIFIED: January 2026 - Added GitHub Integration for Remote Access
 *  MODIFIED: March 2026   - Code review corrections:
 *                           - Removed duplicate jsonbinURL/jsonbinKey declarations
 *                             inside updateJSONBinWeatherData()
 *                           - Fixed indentation of JSONBin update block in loop()
 *                           - JSONBin now only updates when new sensor data arrives
 *                           - Removed propagateToOpenHAB() - OpenHAB not in use
 *                           - Added 5-second timeout to HTTPClient
 *                           - Moved credentials to secrets.h
 * -------------------------------------------------------------------------------- */

#include "secrets.h"         // WiFi credentials and API keys - keep out of version control

#include <Bolbro.h>
#include <BolbroWebServer.h>

#include <HTTPClient.h>
#include <base64.h>

#include <WeatherPacket.h>
#include <CalibrationPacket.h>
#include <HC12.h>

#include "History.h"
#include "DailyMinMax.h"
#include "Sun.h"

//  Forecast configuration
//  To enable: set USEFORECAST to 1, define LATITUDE, LONGITUDE, and set APIKEY in secrets.h
#define USEFORECAST      0
#define FORECASTNUMDAYS  16
#define APIKEY           "APIKEY"

//  JSONBin configuration for remote access
const char* jsonbinURL            = "https://api.jsonbin.io/v3/b/696e511cae596e708fe6da08";
const char* jsonbinKey            = JSONBIN_MASTER_KEY;
unsigned long lastJSONBinUpdate   = 0;
const unsigned long jsonbinUpdateInterval = 60000;

//  Tracks whether JSONBin has received the most recent packet
time_t lastPacketUpdateAtJSONBin  = 0;

//  Weather data
WeatherPacket weatherPacket;
time_t lastPacketUpdate   = 0;
bool stationOffline       = true;
WeatherPacket newWeatherPacket;

//  Configuration data
CalibrationPacket calibrationPacket;

static void sendCalibration() {
  uint8_t *packetBinary = calibrationPacket.encodedBytes();
  int packetSize        = calibrationPacket.encodedSize();

  if (DEBUG) {
    LOG->println("sending calibration...");
    calibrationPacket.print(LOG);
  }

  HC12.write(packetBinary, packetSize);
  calibrationPacket.mCommand = CalibrationPacket::Command::NoCommand;
}

//  Aggregated values
History windHistory("wind",           10 * 60);
History rainHistory("rain",           60 * 60);
History rain24Hour("rain24",      24 * 60 * 60);
History barometricHistory("barometer", 30 * 60);
DailyMinMax temperatureMinMax("temperature");

static void updateAggregates() {
  if (weatherPacket.mTemperatureDegreeCelsius != UNDEFINEDVALUE)
    temperatureMinMax.addSample(weatherPacket.mTemperatureDegreeCelsius);

  if (weatherPacket.mWindSpeedMpS != UNDEFINEDVALUE)
    windHistory.addSample(weatherPacket.mWindSpeedMpS);

  if (weatherPacket.mDeltaRainMM != UNDEFINEDVALUE) {
    rainHistory.addDeltaSample(weatherPacket.mDeltaRainMM);
    rain24Hour.addDeltaSample(weatherPacket.mDeltaRainMM);
  }

  if (weatherPacket.mPressureHPA != UNDEFINEDVALUE)
    barometricHistory.addSample(weatherPacket.mPressureHPA);
}

//  JSONBin update — only called when new weather data has arrived
static void updateJSONBinWeatherData() {
  if (WiFi.status() != WL_CONNECTED) {
    LOG->println("Cannot update JSONBin - WiFi not connected");
    return;
  }

  HTTPClient http;

  // Build weatherdata.json content
  String json = "{\n";
  json += "  \"weather\": {\n";
  json += "    \"temperature\": \""      + String(weatherPacket.mTemperatureDegreeCelsius, 1) + "\",\n";
  json += "    \"humidity\": \""         + String(weatherPacket.mHumidityPercent, 0)          + "\",\n";
  json += "    \"pressure\": \""         + String(weatherPacket.mPressureHPA, 2)              + "\",\n";
  json += "    \"winddirection\": \""    + String(weatherPacket.mWindDirection)               + "\",\n";
  json += "    \"batterypercentage\": \"" + String(weatherPacket.batteryPercentage(), 0)      + "\"\n";
  json += "  },\n";
  json += "  \"aggregated\": {\n";

  if (temperatureMinMax.hasSamples()) {
    json += "    \"mintemperature\": \"" + String(temperatureMinMax.min(), 0) + "\",\n";
    json += "    \"maxtemperature\": \"" + String(temperatureMinMax.max(), 0) + "\",\n";
  } else {
    json += "    \"mintemperature\": \"-\",\n";
    json += "    \"maxtemperature\": \"-\",\n";
  }

  if (barometricHistory.hasSamples())
    json += "    \"barotrend\": \"" + String(barometricHistory.change(), 1) + "\",\n";
  else
    json += "    \"barotrend\": \"-\",\n";

  if (rainHistory.hasSamples())
    json += "    \"rainhour\": \"" + String(rainHistory.range(), 1) + "\",\n";
  else
    json += "    \"rainhour\": \"-\",\n";

  if (rain24Hour.hasSamples())
    json += "    \"rainday\": \"" + String(rain24Hour.range(), 1) + "\",\n";
  else
    json += "    \"rainday\": \"-\",\n";

  if (windHistory.hasSamples()) {
    float windMpS  = windHistory.avg();
    float gustsMpS = windHistory.max();
    json += "    \"windmps\": \""    + String(windMpS  * 1.0,     1) + "\",\n";
    json += "    \"windmph\": \""    + String(windMpS  * 2.23694, 1) + "\",\n";
    json += "    \"windknots\": \""  + String(windMpS  * 1.94384, 1) + "\",\n";
    json += "    \"gustsmps\": \""   + String(gustsMpS * 1.0,     1) + "\",\n";
    json += "    \"gustsmph\": \""   + String(gustsMpS * 2.23694, 1) + "\",\n";
    json += "    \"gustsknots\": \"" + String(gustsMpS * 1.94384, 1) + "\"\n";
  } else {
    json += "    \"windmps\": \"-\",\n";
    json += "    \"windmph\": \"-\",\n";
    json += "    \"windknots\": \"-\",\n";
    json += "    \"gustsmps\": \"-\",\n";
    json += "    \"gustsmph\": \"-\",\n";
    json += "    \"gustsknots\": \"-\"\n";
  }

  json += "  },\n";
  char sunriseStr[16] = "-";
  char sunsetStr[16]  = "-";
  calcSunriseSunset(sunriseStr, sunsetStr, sizeof(sunriseStr));

  json += "  \"sun\": {\n";
  json += "    \"azimuth\": \""     + String(calibrationPacket.mAzimuth,     0) + "\",\n";
  json += "    \"inclination\": \"" + String(calibrationPacket.mInclination, 0) + "\",\n";
  json += "    \"sunrise\": \""     + String(sunriseStr)                        + "\",\n";
  json += "    \"sunset\": \""      + String(sunsetStr)                         + "\"\n";
  json += "  },\n";

  time_t now = time(NULL);
  char timeBuf[64];
  strftime(timeBuf, sizeof(timeBuf), "%a, %b %d, %I:%M %p", localtime(&now));
  json += "  \"updated-de\": \""  + String(timeBuf)                                   + "\",\n";
  json += "  \"offline\": "       + String(stationOffline ? "true" : "false")         + "\n";
  json += "}";

  http.begin(jsonbinURL);
  http.setTimeout(5000);   // 5-second timeout — prevents blocking loop() if JSONBin is slow
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Master-Key", jsonbinKey);

  int httpCode = http.PUT(json);

  if (httpCode == 200)
    LOG->println("JSONBin updated successfully");
  else
    LOG->printf("JSONBin update failed: HTTP %d\n", httpCode);

  http.end();
}

//  Web server class
class WeatherWebServer : public BolbroWebServer {
  public:
    WeatherWebServer() : BolbroWebServer() {}

    void begin() {
      on("/",                         [this]() { loadFromSpiffs("/index.html"); });
      on("/administration.html",      [this]() { CHECKLOCALACCESS loadFromSpiffs("/administration.html"); });
      on("/aztec-background.jpg",       [this]() { loadFromSpiffs("/aztec-background.jpg"); });
      on("/aztec-background-gray.jpg",  [this]() { loadFromSpiffs("/aztec-background-gray.jpg"); });
      on("/cantera-background.jpeg",    [this]() { loadFromSpiffs("/cantera-background.jpeg"); });
      on("/battery.png",              [this]() { loadFromSpiffs("/battery.png"); });
      on("/humidity.png",             [this]() { loadFromSpiffs("/humidity.png"); });
      on("/pressure.png",             [this]() { loadFromSpiffs("/pressure.png"); });
      on("/temperature.png",          [this]() { loadFromSpiffs("/temperature.png"); });
      on("/wind.png",                 [this]() { loadFromSpiffs("/wind.png"); });
      on("/updated.png",              [this]() { loadFromSpiffs("/updated.png"); });
      on("/sun.png",                  [this]() { loadFromSpiffs("/sun.png"); });
      on("/sunrise.png",              [this]() { loadFromSpiffs("/sunrise.png"); });
      on("/rain.png",                 [this]() { loadFromSpiffs("/rain.png"); });
      on("/inclination.png",          [this]() { loadFromSpiffs("/inclination.png"); });
      on("/azimuth.png",              [this]() { loadFromSpiffs("/azimuth.png"); });
      on("/raindrop.png",             [this]() { loadFromSpiffs("/raindrop.png"); });
      on("/WeatherIcon01d.png",       [this]() { loadFromSpiffs("/WeatherIcon01d.png"); });
      on("/WeatherIcon02d.png",       [this]() { loadFromSpiffs("/WeatherIcon02d.png"); });
      on("/WeatherIcon03d.png",       [this]() { loadFromSpiffs("/WeatherIcon03d.png"); });
      on("/WeatherIcon04d.png",       [this]() { loadFromSpiffs("/WeatherIcon04d.png"); });
      on("/WeatherIcon09d.png",       [this]() { loadFromSpiffs("/WeatherIcon09d.png"); });
      on("/WeatherIcon10d.png",       [this]() { loadFromSpiffs("/WeatherIcon10d.png"); });
      on("/WeatherIcon11d.png",       [this]() { loadFromSpiffs("/WeatherIcon11d.png"); });
      on("/WeatherIcon13d.png",       [this]() { loadFromSpiffs("/WeatherIcon13d.png"); });
      on("/WeatherIcon50d.png",       [this]() { loadFromSpiffs("/WeatherIcon50d.png"); });
      on("/windN.png",                [this]() { loadFromSpiffs("/windN.png"); });
      on("/windNNE.png",              [this]() { loadFromSpiffs("/windNNE.png"); });
      on("/windNE.png",               [this]() { loadFromSpiffs("/windNE.png"); });
      on("/windENE.png",              [this]() { loadFromSpiffs("/windENE.png"); });
      on("/windE.png",                [this]() { loadFromSpiffs("/windE.png"); });
      on("/windESE.png",              [this]() { loadFromSpiffs("/windESE.png"); });
      on("/windSE.png",               [this]() { loadFromSpiffs("/windSE.png"); });
      on("/windSSE.png",              [this]() { loadFromSpiffs("/windSSE.png"); });
      on("/windS.png",                [this]() { loadFromSpiffs("/windS.png"); });
      on("/windSSW.png",              [this]() { loadFromSpiffs("/windSSW.png"); });
      on("/windSW.png",               [this]() { loadFromSpiffs("/windSW.png"); });
      on("/windWSW.png",              [this]() { loadFromSpiffs("/windWSW.png"); });
      on("/windW.png",                [this]() { loadFromSpiffs("/windW.png"); });
      on("/windWNW.png",              [this]() { loadFromSpiffs("/windWNW.png"); });
      on("/windNW.png",               [this]() { loadFromSpiffs("/windNW.png"); });
      on("/windNNW.png",              [this]() { loadFromSpiffs("/windNNW.png"); });
      on("/RedDot12.png",             [this]() { loadFromSpiffs("/RedDot12.png"); });
      on("/GreenDot12.png",           [this]() { loadFromSpiffs("/GreenDot12.png"); });
      on("/weatherdata.json",         [this]() { handleWeatherData(); });
      on("/forecast-configuration.json", [this]() { handleForecastConfiguration(); });
      on("/calibrationdata.json",     [this]() { handleCalibrationData(); });
      on("/change-calibration",       [this]() { CHECKLOCALACCESS changeCalibration(); });
      on("/revert-calibration",       [this]() { CHECKLOCALACCESS revertCalibration(); });
      on("/calibrate-tracker",        [this]() { CHECKLOCALACCESS calibrateTracker(); });
      on("/test-tracker",             [this]() { CHECKLOCALACCESS testTracker(); });
      BolbroWebServer::begin();
    }

  private:
    void handleForecastConfiguration() {
      String json = "{\n";
#if USEFORECAST
      json += "\t\"latitude\" : "  + String(LATITUDE,        2) + ",\n";
      json += "\t\"longitude\" : " + String(LONGITUDE,       2) + ",\n";
      json += "\t\"numdays\" : "   + String(FORECASTNUMDAYS)    + ",\n";
      json += "\t\"apikey\" : \""  + String(APIKEY)             + "\"\n";
#endif
      json += "}\n";
      send(200, "application/json", json);
      LOG->println("file /forecast-configuration.json generated and sent");
    }

    void handleWeatherData() {
      String json = "{\n";
      json += "\t\"weather\" : " + weatherPacket.json("\t") + ",\n";
      String message = textMessage();
      if (stationOffline) {
        if (message.length() > 0) message.concat(" ");
        message.concat("Current values are outdated, please check the time of last report.");
      }
      if (message.length() > 0)
        json += "\t\"message\" : \"" + message + "\",\n";
      if (lastPacketUpdate) {
        char timeCStr[32];
        struct tm *t = localtime(&lastPacketUpdate);
        strftime(timeCStr, 31, "%d. %b %X", t);
        String updatedStr(*timeCStr == '0' ? timeCStr + 1 : timeCStr);
        static struct { const char *shortMonth; const char *longMonth; } months[] = {
          { "Jan", "January"  }, { "Feb", "February" }, { "Mar", "March"     }, { "Apr", "April"    },
          { "May", "May"      }, { "Jun", "June"      }, { "Jul", "July"      }, { "Aug", "August"   },
          { "Sep", "September"}, { "Oct", "October"   }, { "Nov", "November"  }, { "Dec", "December" }
        };
        for (int i = 0; i < 12; i++) updatedStr.replace(months[i].shortMonth, months[i].longMonth);
        json += "\t\"updated-de\" : \"" + updatedStr + "\",\n";
        String timeStr(ctime(&lastPacketUpdate));
        timeStr.replace("  ", " "); timeStr.replace("\n", "");
        json += "\t\"updated\" : \"" + timeStr + "\",\n";
      } else {
        json += "\t\"updated-de\" : \"-\",\n";
        json += "\t\"updated\" : \"-\",\n";
      }
      json += "\t\"offline\" : "; json += stationOffline ? "true" : "false"; json += ",\n";
      json += "\t\"sun\" : {\n";
      json += "\t\t\"inclination\" : " + String(calibrationPacket.mInclination, 1) + ",\n";
      json += "\t\t\"azimuth\" : "     + String(calibrationPacket.mAzimuth,     1) + "\n";
      json += "\t},\n";
      json += "\t\"aggregated\" : {\n";
      if (temperatureMinMax.hasSamples()) {
        json += "\t\t\"mintemperature\" : " + String(temperatureMinMax.min(), 1) + ",\n";
        json += "\t\t\"maxtemperature\" : " + String(temperatureMinMax.max(), 1) + ",\n";
      } else {
        json += "\t\t\"mintemperature\" : \"-\",\n";
        json += "\t\t\"maxtemperature\" : \"-\",\n";
      }
      if (windHistory.hasSamples()) {
        float windMpS  = windHistory.avg();
        float gustsMpS = windHistory.max();
        json += "\t\t\"windmps\" : "       + String(windMpS  * 1.0,     1) + ",\n";
        json += "\t\t\"windmph\" : "       + String(windMpS  * 2.23694, 1) + ",\n";
        json += "\t\t\"windknots\" : "     + String(windMpS  * 1.94384, 1) + ",\n";
        json += "\t\t\"windbeaufort\" : "  + String(round(pow(windMpS  / 0.836, 2.0 / 3.0)), 0) + ",\n";
        json += "\t\t\"gustsmps\" : "      + String(gustsMpS * 1.0,     1) + ",\n";
        json += "\t\t\"gustsmph\" : "      + String(gustsMpS * 2.23694, 1) + ",\n";
        json += "\t\t\"gustsknots\" : "    + String(gustsMpS * 1.94384, 1) + ",\n";
        json += "\t\t\"gustsbeaufort\" : " + String(round(pow(gustsMpS / 0.836, 2.0 / 3.0)), 0) + ",\n";
      } else {
        json += "\t\t\"windmps\" : \"-\",\n";      json += "\t\t\"windmph\" : \"-\",\n";
        json += "\t\t\"windknots\" : \"-\",\n";    json += "\t\t\"windbeaufort\" : \"-\",\n";
        json += "\t\t\"gustsmps\" : \"-\",\n";     json += "\t\t\"gustsmph\" : \"-\",\n";
        json += "\t\t\"gustsknots\" : \"-\",\n";   json += "\t\t\"gustsbeaufort\" : \"-\",\n";
      }
      if (barometricHistory.hasSamples())
        json += "\t\t\"barotrend\" : " + String(barometricHistory.change(), 1) + ",\n";
      else
        json += "\t\t\"barotrend\" : \"-\",\n";
      if (rain24Hour.hasSamples())
        json += "\t\t\"rainday\" : " + String(rain24Hour.range(), 1) + ",\n";
      else
        json += "\t\t\"rainday\" : \"-\",\n";
      if (rainHistory.hasSamples())
        json += "\t\t\"rainhour\" : " + String(rainHistory.range(), 1) + "\n";
      else
        json += "\t\t\"rainhour\" : \"-\"\n";
      json += "\t}\n";
      json += "}\n";
      send(200, "application/json", json);
      LOG->println("file /weatherdata.json generated and sent");
    }

    void handleCalibrationData() {
      String json = calibrationPacket.json(textMessage());
      send(200, "application/json", json);
      LOG->println("file /calibrationdata.json generated and sent");
    }

    void revertCalibration() {
      LOG->println(messageToString());
      bool hadErrors = false; bool hadPassword = false;
      for (uint8_t i = 0; i < args(); i++) {
        if (argName(i) == "password") {
          hadErrors   = hadErrors || arg(i) != ADMINPASSWORD;
          hadPassword = true;
          LOG->printf("password: %s, hadErrors: %s\n", arg(i).c_str(), hadErrors ? "true" : "false");
        } else hadErrors = true;
      }
      if (hadErrors || !hadPassword) send(404, "text/plain", "invalid arguments");
      else { calibrationPacket.revertToDefaults(); send(200, "text/plain", "OK"); }
    }

    void changeCalibration() {
      LOG->println(messageToString());
      bool hadErrors = false; bool hadPassword = false;
      for (uint8_t i = 0; i < args(); i++) {
        if (argName(i) == "password") {
          hadErrors   = hadErrors || arg(i) != ADMINPASSWORD;
          hadPassword = true;
          LOG->printf("password: %s, hadErrors: %s\n", arg(i).c_str(), hadErrors ? "true" : "false");
        } else if (argName(i) == "speedFactor") {
          float newValue = arg(i).toFloat();
          if (newValue == 0) hadErrors = true; else calibrationPacket.mWindSpeedFactor = newValue;
          LOG->printf("speedFactor: %.1f, hadErrors: %s\n", newValue, hadErrors ? "true" : "false");
        } else if (argName(i) == "height") {
          float newValue = arg(i).toFloat();
          if (newValue == 0) hadErrors = true; else calibrationPacket.mMeasurementHeight = newValue;
          LOG->printf("height: %.2f, hadErrors: %s\n", newValue, hadErrors ? "true" : "false");
        } else if (argName(i) == "reportSecs") {
          unsigned long newValue = arg(i).toInt();
          if (newValue == 0) hadErrors = true; else calibrationPacket.mSecondsBetweenReports = newValue;
          LOG->printf("reportSecs: %ld, hadErrors: %s\n", newValue, hadErrors ? "true" : "false");
        } else if (argName(i) == "bucketVol") {
          float newValue = arg(i).toFloat();
          if (newValue == 0) hadErrors = true; else calibrationPacket.mBucketTriggerVolume = newValue;
          LOG->printf("bucketVol: %.2f, hadErrors: %s\n", newValue, hadErrors ? "true" : "false");
        } else if (argName(i) == "message") {
          setTextMessage(arg(i)); LOG->printf("message: '%s'\n", arg(i));
        } else hadErrors = true;
      }
      if (hadErrors || !hadPassword) send(404, "text/plain", "invalid arguments");
      else { calibrationPacket.save(); send(200, "text/plain", "OK"); }
    }

    void calibrateTracker() {
      calibrationPacket.mCommand = CalibrationPacket::Command::CalibrateSolarTracker;
      send(200, "text/plain", "OK");
    }

    void testTracker() {
      calibrationPacket.mCommand = CalibrationPacket::Command::TestSolarTracker;
      send(200, "text/plain", "OK");
    }
};

WeatherWebServer server;

static void propagateToOpenHAB() {
  if (weatherPacket.mTemperatureDegreeCelsius != UNDEFINEDVALUE)
    Bolbro.updateItem("ESP32_Weatherbase_Temperature",  String(weatherPacket.mTemperatureDegreeCelsius, 1) + "°C");
  if (weatherPacket.mDeltaRainMM != UNDEFINEDVALUE)
    Bolbro.updateItem("ESP32_Weatherbase_DeltaRain",    String(weatherPacket.mDeltaRainMM, 2) + "mm");
  if (weatherPacket.mPressureHPA != UNDEFINEDVALUE)
    Bolbro.updateItem("ESP32_Weatherbase_Pressure",     String(weatherPacket.mPressureHPA, 0) + "hPa");
  if (weatherPacket.mHumidityPercent != UNDEFINEDVALUE)
    Bolbro.updateItem("ESP32_Weatherbase_Humidity",     String(weatherPacket.mHumidityPercent, 1) + "%");
  if (weatherPacket.mWindDirection[0] != '\0')
    Bolbro.updateItem("ESP32_Weatherbase_WindAngle",    String(weatherPacket.mWindDirection));
  if (weatherPacket.mWindSpeedMpS != UNDEFINEDVALUE)
    Bolbro.updateItem("ESP32_Weatherbase_RawWindStrength", String(weatherPacket.mWindSpeedMpS, 1) + "m/s");
  Bolbro.updateItem("ESP32_Weatherbase_BatteryLevel",   String(weatherPacket.batteryPercentage(), 0) + "%");
  Bolbro.updateItem("ESP32_Weatherbase_BatteryVoltage", String(weatherPacket.mBatteryVoltage, 2) + "V");
  Bolbro.updateItem("ESP32_Weatherbase_LastUpdate",     Bolbro.openHABTime(lastPacketUpdate));
}

void setup() {
  Bolbro.setSerialBaud(115200l);
  Bolbro.addWiFi(WIFI_SSID, WIFI_PASSWORD);   // credentials now sourced from secrets.h
  Bolbro.addWANGateway("WAN");
  Bolbro.setOpenHABHost("openhabian");
  Bolbro.setSignalStrengthItem("ESP32_Weatherbase_SignalStrength");
  Bolbro.setLastStartItem("ESP32_Weatherbase_LastStart");
  Bolbro.setup("Weatherbase", DEBUG, USEREMOTEDEBUG);
  Serial.println("Weather setup...");
  Bolbro.connectToWiFi();
  Bolbro.configureTime();
  calibrationPacket.restore();
  Serial.printf("Base setup...\n");
  HC12.begin();
  pinMode(LED_PIN, OUTPUT);
  server.begin();
  Serial.println("HTTP server started");
  Serial.println("JSONBin weather updates enabled");
}

void loop() {
  static unsigned long lastMillisLEDTurnedOn    = 0;
  static unsigned long lastMillisSunCalculated  = 0;
  static unsigned long lastMillisPacketUpdated  = 0;
  static const char   *stationOnlineStatus      = NULL;
  unsigned long currentMillis = millis();

  server.handleClient();
  delay(10);

  if (HC12.available()) {
    lastMillisLEDTurnedOn = currentMillis;
    digitalWrite(LED_PIN, LOW);
    if (newWeatherPacket.decodeByte(HC12.read())) {
      weatherPacket       = newWeatherPacket;
      weatherPacket.print(LOG);
      lastPacketUpdate    = time(NULL);
      lastMillisPacketUpdated = currentMillis;
      sendCalibration();
      updateAggregates();
    }
  }

  //  Update JSONBin every 60 seconds, but only if new sensor data has arrived since last push
  if (currentMillis - lastJSONBinUpdate >= jsonbinUpdateInterval) {
    if (lastPacketUpdate != lastPacketUpdateAtJSONBin) {
      updateJSONBinWeatherData();
      lastPacketUpdateAtJSONBin = lastPacketUpdate;
    }
    lastJSONBinUpdate = currentMillis;
  }

  unsigned long secondsPassed = (currentMillis - lastMillisLEDTurnedOn) / MS2S_FACTOR;
  if (secondsPassed > 2) digitalWrite(LED_PIN, HIGH);

#define NUMMISSEDPACKETSIGNORED 4
  secondsPassed = (currentMillis - lastMillisPacketUpdated) / MS2S_FACTOR;
  if (lastMillisPacketUpdated)
    stationOffline = secondsPassed > (NUMMISSEDPACKETSIGNORED + 1) * calibrationPacket.mSecondsBetweenReports;
  else
    stationOffline = true;

  if (stationOffline) digitalWrite(LED_PIN, secondsPassed % 2 ? LOW : HIGH);

  if (!stationOnlineStatus || strcmp(stationOnlineStatus, stationOffline ? "OFF" : "ON") != 0) {
    stationOnlineStatus = stationOffline ? "OFF" : "ON";
    Bolbro.updateItem("ESP32_Weatherstation_Status", stationOnlineStatus);
  }

  secondsPassed = (currentMillis - lastMillisSunCalculated) / MS2S_FACTOR;
  if (secondsPassed > 60) {
    calcSun(&calibrationPacket.mAzimuth, &calibrationPacket.mInclination);
    lastMillisSunCalculated = currentMillis;
  }

  Bolbro.loop();
  delay(10);
}