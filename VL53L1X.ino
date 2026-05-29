/*
 * VL53L1X flatline‑triggered nonce stream
 *
 * - Streams distance samples from the VL53L1X.
 * - Detects a flat‑line window.
 * - On first flat‑line detection, prints the current nonce value.
 * - Afterwards, continues to increment and print the nonce each loop.
 *
 * Install the Adafruit_VL53L1X library via Library Manager before compiling.
 */

#include <Wire.h>
#include <Adafruit_VL53L1X.h>

// ---------- VL53L1X ----------
Adafruit_VL53L1X vl53;

// ---------- Flat‑line detection ----------
const uint8_t WINDOW      = 16;               // samples in rolling window
const uint16_t FLATLINE_DELTA_MM = 2;         // max spread to call it flat
const uint8_t  REQUIRED_STABLE_PAIRS = 12;    // consecutive stable pairs needed

struct Sample {
  uint32_t t_ms;
  int16_t  dist_mm;
};

Sample   ring[WINDOW];
uint8_t  ringCount = 0;
uint8_t  ringWrite = 0;
uint32_t sampleCount = 0;

void pushSample(const Sample &s) {
  ring[ringWrite] = s;
  ringWrite = (ringWrite + 1) % WINDOW;
  if (ringCount < WINDOW) ringCount++;
}

Sample getOrdered(uint8_t idx) {
  uint8_t start = (ringCount < WINDOW) ? 0 : ringWrite;
  return ring[(start + idx) % WINDOW];
}

bool detectFlatline(uint16_t &stablePairs, int16_t &minD, int16_t &maxD) {
  if (ringCount < WINDOW) return false;

  minD = 32767;
  maxD = -32768;
  stablePairs = 0;

  // spread
  for (uint8_t i = 0; i < ringCount; i++) {
    Sample s = getOrdered(i);
    if (s.dist_mm < minD) minD = s.dist_mm;
    if (s.dist_mm > maxD) maxD = s.dist_mm;
  }

  // consecutive stability
  for (uint8_t i = 1; i < ringCount; i++) {
    Sample a = getOrdered(i - 1);
    Sample b = getOrdered(i);
    if (abs(b.dist_mm - a.dist_mm) <= (int16_t)FLATLINE_DELTA_MM) {
      stablePairs++;
    }
  }

  return ((maxD - minD) <= (int16_t)FLATLINE_DELTA_MM) &&
         (stablePairs >= REQUIRED_STABLE_PAIRS);
}

// ---------- Nonce ----------
uint32_t nonce = 0;
bool     flatlineEmitted = false;   // true after we have sent the first flat‑line nonce

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }

  Wire.begin();
  Wire.setClock(400000);

  Serial.println(F("# VL53L1X flatline nonce stream"));
  Serial.println(F("# t_ms,sample,distance_mm,flatline,stable_pairs,nonce"));

  if (!vl53.begin()) {
    Serial.println(F("ERROR: VL53L1X not detected!"));
    while (1) { delay(1000); }
  }

  vl53.startRanging();
}

void loop() {
  // read a distance sample if ready
  if (vl53.dataReady()) {
    int16_t dist = vl53.distance();
    vl53.clearInterrupt();

    Sample s;
    s.t_ms   = millis();
    s.dist_mm = dist;
    pushSample(s);
    sampleCount++;

    uint16_t stablePairs = 0;
    int16_t minD = 0, maxD = 0;
    bool isFlat = detectFlatline(stablePairs, minD, maxD);

    // On the *first* flat‑line detection, emit the current nonce
    if (isFlat && !flatlineEmitted) {
      flatlineEmitted = true;
      Serial.print(F("# FIRST_FLATLINE_AT="));
      Serial.println(nonce);
    }

    // Always stream the current nonce (after the first flat‑line it will just keep counting)
    Serial.print(s.t_ms);
    Serial.print(',');
    Serial.print(sampleCount);
    Serial.print(',');
    Serial.print(s.dist_mm);
    Serial.print(',');
    Serial.print(isFlat ? 1 : 0);
    Serial.print(',');
    Serial.print(stablePairs);
    Serial.print(',');
    Serial.println(nonce);

    // increment nonce for next iteration
    nonce++;
  }

  // small delay to avoid hogging the CPU; adjust as needed
  delay(1);
}
