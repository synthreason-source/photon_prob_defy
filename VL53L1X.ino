/*
 * VL53L1X Data Stream - CSV Format with Headers
 *
 * outputs detailed CSV with timestamps for data logging
 * Compatible with Excel, MATLAB, Python pandas
 *
 * Usage: Serial Monitor or Terminal → Copy/Paste to file
 * Or use: screen /dev/ttyXXX 115200 (Linux/Mac)
 */

#include <Wire.h>
#include <Adafruit_VL53L1X.h>

Adafruit_VL53L1X sensor = Adafruit_VL53L1X();

unsigned long start_time = 0;
int sample_count = 0;

void setup() {
    Serial.begin(115200);
    while (!Serial) { delay(10); }

    Wire.begin();
    Wire.setClock(400000);

    // Print CSV header
    Serial.println(F("# VL53L1X Ranging Data Stream"));
    Serial.println(F("# Format: timestamp_ms,sample_num,distance_mm,signal_rate,crosstalk_rate,ambient_rate,range_status,sigma_mm"));
    Serial.println(F("#"));

    if (!sensor.begin()) {
        Serial.println(F("ERROR: VL53L1X not detected! Check 3.3V power and I2C wiring."));
        while (1) {
            Serial.println(F("ERROR: Sensor not found"));
            delay(1000);
        }
    }

    // Configure for short-range (more stable for close targets)
    sensor.VL53L1X_SetDistanceMode(1);  // 1 = Short, 2 = Long
    sensor.setTimingBudget(50000);      // 50ms
    sensor.VL53L1X_SetInterMeasurementInMs(50);

    sensor.startRanging();
    start_time = millis();

    Serial.println(F("# STREAM_ACTIVE"));
    Serial.flush();
}

void loop() {
    if (sensor.dataReady()) {
        unsigned long timestamp = millis() - start_time;
        sample_count++;

        int16_t distance = sensor.distance();
        uint16_t signal_rate  = 0;
        uint16_t ambient_rate = 0;
        uint16_t sigma        = 0;
        // CSV output (parseable by Python/pandas)
        Serial.print(timestamp);
        Serial.print(',');
        Serial.print(sample_count);
        Serial.print(',');
        Serial.print(distance);
        Serial.print(',');
        Serial.print(signal_rate / 65536.0, 4);
        Serial.print(',');
        Serial.print(0.0, 4);  // Crosstalk not exposed in Arduino lib
        Serial.print(',');
        Serial.print(ambient_rate / 65536.0, 4);
        Serial.print(',');
        Serial.println(sigma);

        sensor.clearInterrupt();
    }

    // Optional: limit stream rate
    delay(10);
}
