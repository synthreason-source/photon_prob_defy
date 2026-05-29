#include <Wire.h>
#include <Adafruit_VL53L1X.h>
#include <SHA256.h>

Adafruit_VL53L1X vl53;
SHA256 sha256;

// ── Window / flatline config ──────────────────────────────────────────────────
const uint8_t  WINDOW                = 4;
const uint16_t FLATLINE_DELTA_MM     = 32800;
const uint8_t  REQUIRED_STABLE_PAIRS = 2;

// ── PoW config ────────────────────────────────────────────────────────────────
const uint8_t DIFFICULTY_BITS = 4;

// ── Mode flag ─────────────────────────────────────────────────────────────────
bool useSHA256 = true;

// ── Ring buffer ───────────────────────────────────────────────────────────────
struct Sample { uint32_t t_ms; int16_t dist_mm; };
Sample  ringBuf[WINDOW];
uint8_t ringCount = 0;
uint8_t ringWrite = 0;

// ── State ─────────────────────────────────────────────────────────────────────
uint32_t powSeed       = 0;
uint32_t powNonce      = 0;
bool     flatlineState = false;
uint8_t  lastConfidence = 0;   // 0–100, printed on every change

// ─────────────────────────────────────────────────────────────────────────────

void pushSample(const Sample &s) {
  ringBuf[ringWrite] = s;
  ringWrite = (ringWrite + 1) % WINDOW;
  if (ringCount < WINDOW) ringCount++;
}

Sample getOrdered(uint8_t idx) {
  uint8_t start = (ringCount < WINDOW) ? 0 : ringWrite;
  return ringBuf[(start + idx) % WINDOW];
}

// ── Confidence score ──────────────────────────────────────────────────────────
// Two sub-scores are blended 50/50:
//
//  SPREAD score  (0–100):
//    How tight the overall min–max range is across the window.
//    spread=0mm → 100,  spread≥FLATLINE_DELTA_MM → 0.
//    Linear interpolation between those endpoints.
//
//  PAIR score  (0–100):
//    Fraction of consecutive pairs that are within FLATLINE_DELTA_MM.
//    All pairs stable → 100,  no pairs stable → 0.
//
//  Final confidence = (spreadScore + pairScore) / 2
//
//  A reading ≥ 50 is considered a flatline (isFlat = true).
//
uint8_t computeConfidence(bool &isFlat) {
  if (ringCount < WINDOW) {
    isFlat = false;
    return 0;
  }

  // --- spread sub-score ---
  int16_t minD = 32767, maxD = -32768;
  for (uint8_t i = 0; i < ringCount; i++) {
    int16_t d = getOrdered(i).dist_mm;
    if (d < minD) minD = d;
    if (d > maxD) maxD = d;
  }
  int16_t spread = maxD - minD;                      // 0 … ∞ mm
  uint8_t spreadScore;
  if (spread <= 0) {
    spreadScore = 100;
  } else if (spread >= (int16_t)FLATLINE_DELTA_MM) {
    spreadScore = 0;
  } else {
    // map [0, FLATLINE_DELTA_MM] → [100, 0]
    spreadScore = (uint8_t)(100 - (100UL * spread / FLATLINE_DELTA_MM));
  }

  // --- pair sub-score ---
  uint8_t totalPairs  = ringCount - 1;   // WINDOW-1 = 3
  uint8_t stablePairs = 0;
  for (uint8_t i = 1; i < ringCount; i++) {
    int16_t a = getOrdered(i - 1).dist_mm;
    int16_t b = getOrdered(i).dist_mm;
    if (abs(b - a) <= (int16_t)FLATLINE_DELTA_MM) stablePairs++;
  }
  uint8_t pairScore = (uint8_t)(100UL * stablePairs / totalPairs);

  // --- blend ---
  uint8_t confidence = (uint8_t)(((uint16_t)spreadScore + pairScore) / 2);

  // --- flatline decision: threshold at 50 ---
  isFlat = (confidence >= 50);
  return confidence;
}

uint8_t leadingZeroBits(const uint8_t *digest) {
  uint8_t bits = 0;
  for (int i = 0; i < 32; ++i) {
    uint8_t b = digest[i];
    for (int bit = 7; bit >= 0; --bit) {
      if ((b >> bit) & 1) return bits;
      ++bits;
    }
  }
  return bits;
}

void u32ToBE(uint32_t v, uint8_t *out) {
  out[0] = (v >> 24) & 0xFF;
  out[1] = (v >> 16) & 0xFF;
  out[2] = (v >>  8) & 0xFF;
  out[3] =  v        & 0xFF;
}

uint32_t newSeed() {
  randomSeed((uint32_t)analogRead(0) * analogRead(1) *
             (uint32_t)analogRead(2) * analogRead(3) * analogRead(4));
  return (uint32_t)random(1000000000);
}

void printMode() {
  Serial.print(F("# MODE="));
  Serial.println(useSHA256 ? F("SHA256") : F("RAW"));
}

void handleSerial() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim(); cmd.toLowerCase();

  if (cmd == "sha" || cmd == "1") {
    useSHA256 = true;  powNonce = 0;
    Serial.println(F("# CMD: switched to SHA256 mode")); printMode();
  } else if (cmd == "raw" || cmd == "0") {
    useSHA256 = false; powNonce = 0;
    Serial.println(F("# CMD: switched to RAW mode"));    printMode();
  } else if (cmd == "mode") {
    printMode();
  } else {
    Serial.print(F("# UNKNOWN_CMD: ")); Serial.println(cmd);
    Serial.println(F("# valid commands: sha | 1   raw | 0   mode"));
  }
}

// ─────────────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  while (!Serial) {}
  Wire.begin();
  Wire.setClock(400000);

  if (!vl53.begin()) {
    Serial.println(F("ERROR: VL53L1X not detected!"));
    while (1) delay(1000);
  }
  vl53.startRanging();

  powSeed  = newSeed();
  powNonce = 0;

  Serial.print(F("# INITIAL_SEED=")); Serial.println(powSeed);
  Serial.println(F("FLATLINE=FALSE confidence=0"));
  printMode();
  Serial.println(F("# ready  (send 'sha'/'1' or 'raw'/'0' to toggle mode)"));
}

void loop() {
  handleSerial();

  // ── Sensor read ───────────────────────────────────────────────────────────
  if (vl53.dataReady()) {
    int16_t dist = vl53.distance();
    vl53.clearInterrupt();

    Sample s = { millis(), dist };
    pushSample(s);

    bool    nowFlat;
    uint8_t conf = computeConfidence(nowFlat);

    // ── Report on boolean transition OR any confidence change ─────────────
        // Always update both — never gate one on the other
    bool transition = (nowFlat != flatlineState);
    bool confChange = (conf != lastConfidence);

    if (transition && nowFlat) {
      // FALSE → TRUE: reseed
      powSeed  = newSeed();
      powNonce = 0;
      Serial.print(F("# RESEED=")); Serial.println(powSeed);
    }

    flatlineState  = nowFlat;
    lastConfidence = conf;

    if (transition || confChange) {
      Serial.print(F("FLATLINE="));
      Serial.print(flatlineState ? F("TRUE") : F("FALSE"));
      Serial.print(F(" confidence="));
      Serial.println(conf);
    }
  }

  // ── Mine one nonce ────────────────────────────────────────────────────────
  if (useSHA256) {
    uint8_t msg[16];
    memcpy(msg, "flatline", 8);
    u32ToBE(powSeed,  msg + 8);
    u32ToBE(powNonce, msg + 12);

    uint8_t hash[32];
    sha256.reset();
    sha256.update(msg, sizeof(msg));
    sha256.finalize(hash, sizeof(hash));

    uint8_t score = leadingZeroBits(hash);
    if (score >= DIFFICULTY_BITS) {
      Serial.print(F("ZERO_FOUND nonce="));  Serial.print(powNonce);
      Serial.print(F(" score="));            Serial.print(score);
      Serial.print(F(" mode=SHA256 flatline="));
      Serial.print(flatlineState ? F("TRUE") : F("FALSE"));
      Serial.print(F(" confidence="));       Serial.print(lastConfidence);
      Serial.print(F(" hash="));
      for (int i = 0; i < 32; ++i) {
        if (hash[i] < 0x10) Serial.print('0');
        Serial.print(hash[i], HEX);
      }
      Serial.println();
    }

  } else {
    // RAW mode: print every single nonce, no hashing
    Serial.print(F("NONCE nonce="));
    Serial.print(powNonce);
    Serial.print(F(" mode=RAW flatline="));
    Serial.print(flatlineState ? F("TRUE") : F("FALSE"));
    Serial.print(F(" confidence="));
    Serial.println(lastConfidence);
  }

  powNonce++;
  if (powNonce == 0) {
    powSeed = newSeed();
    Serial.print(F("# NONCE_WRAP_RESEED=")); Serial.println(powSeed);
  }
}
