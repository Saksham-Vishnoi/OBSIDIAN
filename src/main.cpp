/*
 * ESP8266 NodeMCU - Temperature Fan Controller with Current Monitoring
 * =====================================================================
 * OLED Display  : SDA -> D0 (GPIO16), SCL -> D1 (GPIO5)
 * Temp Sensor   : Analog -> A0
 * ACS Current   : Analog -> D8 (GPIO15) [Note: Use voltage divider for >3.3V]
 * Relay (Fan)   : D2 (GPIO4)
 *
 * Libraries Required:
 *   - Adafruit SSD1306
 *   - Adafruit GFX
 *   - Wire (built-in)
 *
 * Install via Arduino Library Manager
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ── OLED Config ────────────────────────────────────────────────────────────────
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1   // No reset pin
#define OLED_SDA        4   // D2
#define OLED_SCL         5   // D1

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ── Pin Definitions ───────────────────────────────────────────────────────────
#define RELAY_PIN       16  // D0  (GPIO4)  - Active LOW relay
#define CURRENT_PIN     15   // D8  (GPIO15) - ACS712 output

// Temp sensor on A0 (NodeMCU only has 1 ADC → A0)
// ACS current is read via A0 in a time-multiplexed manner OR
// use a simple voltage divider + separate read cycle.
// ** Since NodeMCU has only ONE analog pin (A0), we alternate reads.
// Connect BOTH sensors via a mux, OR use the approach below:
//   - Temp sensor: directly to A0
//   - ACS712: read digitally via D8 (requires additional circuit for proper reading)
//   - For simplicity, temp on A0 and ACS monitored via A0 with switching
// ** Best practice: use an analog mux (CD4051) or software toggling via digital pins

// ── Temperature Settings ──────────────────────────────────────────────────────
#define TEMP_THRESHOLD  30.0   // °C — Fan turns ON above this temperature
#define TEMP_HYSTERESIS  2.0   // °C — Fan turns OFF below (threshold - hysteresis)

// ── NTC Thermistor Config (if using NTC; adjust for LM35 below) ───────────────
// Comment/uncomment the sensor type you are using
#define SENSOR_LM35          // LM35: 10mV/°C, directly proportional
// #define SENSOR_NTC        // NTC Thermistor: requires Steinhart-Hart equation

#ifdef SENSOR_NTC
  #define NOMINAL_RESISTANCE  10000   // NTC nominal resistance at 25°C
  #define NOMINAL_TEMP           25   // Nominal temperature (°C)
  #define B_COEFFICIENT        3950   // Beta coefficient
  #define SERIES_RESISTOR     10000   // Series resistor value
#endif

// ── ACS712 Config ─────────────────────────────────────────────────────────────
// ACS712-05B: 185 mV/A  |  ACS712-20A: 100 mV/A  |  ACS712-30A: 66 mV/A
#define ACS_SENSITIVITY     0.185    // V/A — change for your ACS712 variant
#define ACS_VCC             3.3      // NodeMCU runs at 3.3V
#define ACS_ADC_RESOLUTION  1023.0
#define ACS_ZERO_OFFSET     512      // ADC value at 0A (Vcc/2) — calibrate this!

// ── Globals ───────────────────────────────────────────────────────────────────
float temperature    = 0.0;
float currentAmps    = 0.0;
bool  fanState       = false;
bool  sensorMode     = false;  // false=temp, true=current (for single-ADC switching)

unsigned long lastSensorRead  = 0;
unsigned long lastDisplayUpdate = 0;
const unsigned long SENSOR_INTERVAL  = 500;   // ms between sensor reads
const unsigned long DISPLAY_INTERVAL = 1000;  // ms between display updates

// ── Function Prototypes ───────────────────────────────────────────────────────
float readTemperature();
float readCurrent();
void  controlFan();
void  updateDisplay();
void  drawBootScreen();

// =============================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n[BOOT] ESP8266 Fan Controller Starting...");

  // Pin Modes
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);  // Relay OFF (active LOW)
  pinMode(CURRENT_PIN, INPUT);

  // I2C on custom pins (SDA=D0/GPIO16, SCL=D1/GPIO5)
  Wire.begin(OLED_SDA, OLED_SCL);

  // OLED Init
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[ERROR] OLED not found! Check wiring.");
    while (true) delay(500);  // Halt
  }

  drawBootScreen();
  delay(2500);
  display.clearDisplay();

  Serial.println("[INFO] System ready.");
  Serial.printf("[INFO] Fan threshold: %.1f°C | Hysteresis: %.1f°C\n",
                TEMP_THRESHOLD, TEMP_HYSTERESIS);
}

// =============================================================================
void loop() {
  unsigned long now = millis();

  // ── Read Sensors ──────────────────────────────────────────────────────────
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;

    temperature = readTemperature();
    currentAmps = readCurrent();

    // Fan Control Logic
    controlFan();

    // Serial Monitor Output
    Serial.printf("[DATA] Temp: %.2f°C | Current: %.3f A | Fan: %s\n",
                  temperature, currentAmps, fanState ? "ON" : "OFF");
  }

  // ── Update Display ────────────────────────────────────────────────────────
  if (now - lastDisplayUpdate >= DISPLAY_INTERVAL) {
    lastDisplayUpdate = now;
    updateDisplay();
  }
}

// =============================================================================
// Read Temperature from LM35 or NTC Thermistor on A0
// =============================================================================
float readTemperature() {
  int rawADC = analogRead(A0);

#ifdef SENSOR_LM35
  // LM35: Output = 10mV/°C, referenced to 3.3V (NodeMCU ADC range: 0–1V via divider)
  // NodeMCU A0 pin has an internal voltage divider: max input = 3.3V → 1V ADC input
  // LM35 output at 100°C = 1.0V → ADC = 1023
  float voltage   = (rawADC / 1023.0) * 3.3;  // Scale back to actual voltage
  float tempC     = voltage * 100.0;            // LM35: 10mV/°C = 100 * voltage(V)
  return tempC;

#elif defined(SENSOR_NTC)
  // Steinhart-Hart for NTC thermistor
  float resistance = SERIES_RESISTOR * ((1023.0 / rawADC) - 1.0);
  float steinhart  = resistance / NOMINAL_RESISTANCE;
  steinhart        = log(steinhart);
  steinhart       /= B_COEFFICIENT;
  steinhart       += 1.0 / (NOMINAL_TEMP + 273.15);
  float tempK      = 1.0 / steinhart;
  return tempK - 273.15;

#else
  // Generic: linear 0–100°C mapped to 0–3.3V
  float voltage = (rawADC / 1023.0) * 3.3;
  return voltage * (100.0 / 3.3);
#endif
}

// =============================================================================
// Read Current from ACS712 — Note: NodeMCU has only A0
// For D8/GPIO15 reading, use an analog breakout or mux.
// This implementation reads a stored ADC value (wire ACS output to A0 with mux,
// or keep ACS on A0 and temp sensor with a digital enable/mux circuit).
//
// *** If your circuit has ONLY ACS on D8 as a digital threshold alert ***
// *** (comparator output), treat it as a digital read:                 ***
// =============================================================================
float readCurrent() {
  // Option A: ACS712 output read via A0 (when mux selects current channel)
  // Since we have one ADC, use a CD4051 mux and toggle a select pin here.
  // For simplicity — if ACS is on A0 and temp sensor uses separate circuit:
  int rawADC = analogRead(A0);

  // Voltage at ACS output pin
  float voltage     = (rawADC / ACS_ADC_RESOLUTION) * ACS_VCC;
  float zeroVoltage = (ACS_ZERO_OFFSET / ACS_ADC_RESOLUTION) * ACS_VCC;

  // Current = (Vmeasured - Vzero) / Sensitivity
  float current = (voltage - zeroVoltage) / ACS_SENSITIVITY;

  // Return absolute value (direction-independent)
  return fabs(current);
}

// =============================================================================
// Fan Control Logic with Hysteresis
// =============================================================================
void controlFan() {
  if (!fanState && temperature >= TEMP_THRESHOLD) {
    // Temperature exceeded threshold → turn fan ON
    fanState = true;
    digitalWrite(RELAY_PIN, LOW);   // Active LOW relay: LOW = Relay ON
    Serial.printf("[RELAY] Fan ON  → Temp %.2f°C >= threshold %.1f°C\n",
                  temperature, TEMP_THRESHOLD);
  }
  else if (fanState && temperature < (TEMP_THRESHOLD - TEMP_HYSTERESIS)) {
    // Temperature dropped below (threshold - hysteresis) → turn fan OFF
    fanState = false;
    digitalWrite(RELAY_PIN, HIGH);  // Active LOW relay: HIGH = Relay OFF
    Serial.printf("[RELAY] Fan OFF → Temp %.2f°C < %.1f°C\n",
                  temperature, TEMP_THRESHOLD - TEMP_HYSTERESIS);
  }
}

// =============================================================================
// OLED Display Update
// =============================================================================
void updateDisplay() {
  display.clearDisplay();

  // ── Header Bar ──────────────────────────────────────────────────────────
  display.fillRect(0, 0, SCREEN_WIDTH, 12, WHITE);
  display.setTextColor(BLACK);
  display.setTextSize(1);
  display.setCursor(20, 2);
  display.print("FAN CONTROLLER v1.0");

  // ── Temperature ─────────────────────────────────────────────────────────
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 16);
  display.print("TEMP:");
  display.setTextSize(2);
  display.setCursor(38, 13);
  display.printf("%.1f", temperature);
  display.setTextSize(1);
  display.setCursor(104, 14);
  display.print("\xB0""C");   // Degree symbol

  // Threshold indicator bar
  display.setCursor(0, 30);
  display.print("LIM:");
  display.printf("%.0f\xB0""C", TEMP_THRESHOLD);

  // Temperature bar (visual)
  int barWidth = (int)((temperature / 50.0) * 60);   // Max 50°C = full bar
  barWidth = constrain(barWidth, 0, 60);
  display.drawRect(64, 30, 62, 8, WHITE);
  display.fillRect(64, 30, barWidth, 8, WHITE);

  // ── Current ─────────────────────────────────────────────────────────────
  display.setCursor(0, 43);
  display.print("CURRENT:");
  display.setTextSize(2);
  display.setCursor(62, 40);
  display.printf("%.2f", currentAmps);
  display.setTextSize(1);
  display.setCursor(112, 41);
  display.print("A");

  // ── Fan Status ──────────────────────────────────────────────────────────
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

// =============================================================================
// Boot Screen Animation
// =============================================================================
void drawBootScreen() {
  display.clearDisplay();

  // Outer frame
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

/*
 * ============================================================================
 * WIRING SUMMARY
 * ============================================================================
 *
 *  NodeMCU       Component           Notes
 *  ─────────     ─────────────────   ──────────────────────────────────────
 *  D0 (GPIO16)   OLED SDA            I2C Data
 *  D1 (GPIO5)    OLED SCL            I2C Clock
 *  3.3V          OLED VCC
 *  GND           OLED GND
 *
 *  A0            LM35 / NTC Output   Use voltage divider if >1V output
 *                                    LM35: VCC=3.3V, OUT→A0, GND→GND
 *
 *  D8 (GPIO15)   ACS712 OUT          Requires analog read via mux OR
 *                                    use comparator for digital alert
 *  3.3V          ACS712 VCC          ACS712 works best at 5V;
 *                                    use level shifter with NodeMCU
 *  GND           ACS712 GND
 *
 *  D2 (GPIO4)    Relay IN            Active LOW; add flyback diode
 *  5V            Relay VCC           Use external 5V for relay coil
 *  GND           Relay GND
 *
 * ============================================================================
 * IMPORTANT NOTES
 * ============================================================================
 *  1. NodeMCU A0 accepts 0–1V (internal divider). Place a 100K:220K divider
 *     before A0 if sensor output exceeds 1V.
 *
 *  2. ACS712 operates at 5V for best accuracy. Use a 3.3V level shifter or
 *     voltage divider on its output before connecting to NodeMCU.
 *
 *  3. D8 (GPIO15) must be LOW at boot. Use a pull-down (10K to GND).
 *     Connect ACS output through a voltage divider to keep it safe.
 *
 *  4. Relay module: Most relay boards are active LOW (IN=LOW → relay ON).
 *     Code uses LOW to turn fan ON, HIGH to turn fan OFF.
 *
 *  5. For independent ADC reads of both temp & current sensors, use:
 *     - A CD4051 8:1 analog mux with a digital select pin
 *     - Then call analogRead(A0) after switching the mux channel
 * ============================================================================
 */
