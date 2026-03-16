#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1
#define OLED_SDA        4
#define OLED_SCL         5

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#define RELAY_PIN       16
#define CURRENT_PIN     15

#define TEMP_THRESHOLD  30.0
#define TEMP_HYSTERESIS  2.0

#define SENSOR_LM35

#ifdef SENSOR_NTC
  #define NOMINAL_RESISTANCE  10000
  #define NOMINAL_TEMP           25
  #define B_COEFFICIENT        3950
  #define SERIES_RESISTOR     10000
#endif

#define ACS_SENSITIVITY     0.185
#define ACS_VCC             3.3
#define ACS_ADC_RESOLUTION  1023.0
#define ACS_ZERO_OFFSET     512

float temperature    = 0.0;
float currentAmps    = 0.0;
bool  fanState       = false;
bool  sensorMode     = false;

unsigned long lastSensorRead  = 0;
unsigned long lastDisplayUpdate = 0;
const unsigned long SENSOR_INTERVAL  = 500;
const unsigned long DISPLAY_INTERVAL = 1000;

float readTemperature();
float readCurrent();
void  controlFan();
void  updateDisplay();
void  drawBootScreen();

void setup() {
  Serial.begin(115200);
  Serial.println("\n[BOOT] ESP8266 Fan Controller Starting...");

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  pinMode(CURRENT_PIN, INPUT);

  Wire.begin(OLED_SDA, OLED_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[ERROR] OLED not found! Check wiring.");
    while (true) delay(500);
  }

  drawBootScreen();
  delay(2500);
  display.clearDisplay();

  Serial.println("[INFO] System ready.");
  Serial.printf("[INFO] Fan threshold: %.1f°C | Hysteresis: %.1f°C\n",
                TEMP_THRESHOLD, TEMP_HYSTERESIS);
}

void loop() {
  unsigned long now = millis();

  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;

    temperature = readTemperature();
    currentAmps = readCurrent();

    controlFan();

    Serial.printf("[DATA] Temp: %.2f°C | Current: %.3f A | Fan: %s\n",
                  temperature, currentAmps, fanState ? "ON" : "OFF");
  }

  if (now - lastDisplayUpdate >= DISPLAY_INTERVAL) {
    lastDisplayUpdate = now;
    updateDisplay();
  }
}

float readTemperature() {
  int rawADC = analogRead(A0);

#ifdef SENSOR_LM35
  float voltage   = (rawADC / 1023.0) * 3.3;
  float tempC     = voltage * 100.0;
  return tempC;

#elif defined(SENSOR_NTC)
  float resistance = SERIES_RESISTOR * ((1023.0 / rawADC) - 1.0);
  float steinhart  = resistance / NOMINAL_RESISTANCE;
  steinhart        = log(steinhart);
  steinhart       /= B_COEFFICIENT;
  steinhart       += 1.0 / (NOMINAL_TEMP + 273.15);
  float tempK      = 1.0 / steinhart;
  return tempK - 273.15;

#else
  float voltage = (rawADC / 1023.0) * 3.3;
  return voltage * (100.0 / 3.3);
#endif
}

float readCurrent() {
  int rawADC = analogRead(A0);

  float voltage     = (rawADC / ACS_ADC_RESOLUTION) * ACS_VCC;
  float zeroVoltage = (ACS_ZERO_OFFSET / ACS_ADC_RESOLUTION) * ACS_VCC;

  float current = (voltage - zeroVoltage) / ACS_SENSITIVITY;

  return fabs(current);
}

void controlFan() {
  if (!fanState && temperature >= TEMP_THRESHOLD) {
    fanState = true;
    digitalWrite(RELAY_PIN, LOW);
    Serial.printf("[RELAY] Fan ON  → Temp %.2f°C >= threshold %.1f°C\n",
                  temperature, TEMP_THRESHOLD);
  }
  else if (fanState && temperature < (TEMP_THRESHOLD - TEMP_HYSTERESIS)) {
    fanState = false;
    digitalWrite(RELAY_PIN, HIGH);
    Serial.printf("[RELAY] Fan OFF → Temp %.2f°C < %.1f°C\n",
                  temperature, TEMP_THRESHOLD - TEMP_HYSTERESIS);
  }
}

void updateDisplay() {
  display.clearDisplay();

  display.fillRect(0, 0, SCREEN_WIDTH, 12, WHITE);
  display.setTextColor(BLACK);
  display.setTextSize(1);
  display.setCursor(20, 2);
  display.print("FAN CONTROLLER v1.0");

  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 16);
  display.print("TEMP:");
  display.setTextSize(2);
  display.setCursor(38, 13);
  display.printf("%.1f", temperature);
  display.setTextSize(1);
  display.setCursor(104, 14);
  display.print("\xB0""C");

  display.setCursor(0, 30);
  display.print("LIM:");
  display.printf("%.0f\xB0""C", TEMP_THRESHOLD);

  int barWidth = (int)((temperature / 50.0) * 60);
  barWidth = constrain(barWidth, 0, 60);
  display.drawRect(64, 30, 62, 8, WHITE);
  display.fillRect(64, 30, barWidth, 8, WHITE);

  display.setCursor(0, 43);
  display.print("CURRENT:");
  display.setTextSize(2);
  display.setCursor(62, 40);
  display.printf("%.2f", currentAmps);
  display.setTextSize(1);
  display.setCursor(112, 41);
  display.print("A");

  display.setCursor(0, 56);
  if (fanState) {
    display.fillRect(0, 55, 128, 9, WHITE);
    display.setTextColor(BLACK);
    display.setCursor(30, 56);
    display.print(">>> FAN ON <<<");
    display.setTextColor(WHITE);
  } else {
    display.print("FAN: STANDBY");
    display.setCursor(75, 56);
    display.printf("T>%.0f\xB0 ON", TEMP_THRESHOLD);
  }

  display.display();
}

void drawBootScreen() {
  display.clearDisplay();

  display.drawRect(0, 0, 128, 64, WHITE);
  display.drawRect(2, 2, 124, 60, WHITE);

  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(22, 8);
  display.print("SMART FAN CTRL");

  display.drawLine(10, 20, 118, 20, WHITE);

  display.setTextSize(1);
  display.setCursor(12, 25);
  display.print("Temp Sensor : A0");
  display.setCursor(12, 34);
  display.print("ACS Current : D8");
  display.setCursor(12, 43);
  display.print("Relay (Fan) : D2");

  display.drawLine(10, 54, 118, 54, WHITE);
  display.setCursor(35, 56);
  display.print("Initializing...");

  display.display();
}
