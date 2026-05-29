#include <Wire.h>
#include <Adafruit_VL53L1X.h>
#include <SHA256.h>

Adafruit_VL53L1X vl53;
SHA256 sha256;

const uint8_t WINDOW = 4;
const uint16_t FLATLINE_DELTA_MM = 20;
const uint8_t REQUIRED_STABLE_PAIRS = 2;

const uint8_t DIFFICULTY_BITS = 16;

struct Sample {
  uint32_t t_ms;
  int16_t dist_mm;
};

Sample ringBuf[WINDOW];
uint8_t ringCount = 0;
uint8_t ringWrite = 0;
uint32_t sampleCount = 0;
uint16_t stablePairsGlobal = 0;

uint32_t powSeed = 0;
uint32_t powNonce = 0;
bool powActive = false;

void pushSample(const Sample &s) {
  ringBuf[ringWrite] = s;
  ringWrite = (ringWrite + 1) % WINDOW;
  if (ringCount < WINDOW) ringCount++;
}

Sample getOrdered(uint8_t idx) {
  uint8_t start = (ringCount < WINDOW) ? 0 : ringWrite;
  return ringBuf[(start + idx) % WINDOW];
}

bool detectFlatline(uint16_t &stablePairs, int16_t &minD, int16_t &maxD) {
  if (ringCount < WINDOW) return false;

  minD = 32767;
  maxD = -32768;
  stablePairs = 0;

  for (uint8_t i = 0; i < ringCount; i++) {
    Sample s = getOrdered(i);
    if (s.dist_mm < minD) minD = s.dist_mm;
    if (s.dist_mm > maxD) maxD = s.dist_mm;
  }

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

uint32_t buildSeedFromWindow() {
  Sample s0 = getOrdered(0);
  randomSeed(analogRead(0)); 

  uint32_t seed = random(1000000000);
 
  return seed;
}

void resetPow() {
  powSeed = 0;
  powNonce = 0;
  powActive = false;
}

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
  Serial.println(F("# ready"));
}

void loop() {
  if (!vl53.dataReady()) return;

  int16_t dist = vl53.distance();
  vl53.clearInterrupt();

  Sample s;
  s.t_ms = millis();
  s.dist_mm = dist;
  pushSample(s);
  sampleCount++;

  uint16_t stablePairs = 0;
  int16_t minD = 0, maxD = 0;
  bool isFlat = detectFlatline(stablePairs, minD, maxD);
  stablePairsGlobal = stablePairs;

  if (isFlat && !powActive) {
    powSeed = buildSeedFromWindow();
    powNonce = 0;
    powActive = true;
    Serial.print(F("# NEW_FLATLINE_SEED="));
    Serial.println(powSeed);
  }

  if (powActive) {
    uint8_t msg[16];
    memcpy(msg, "flatline", 8);
    u32ToBE(powSeed, msg + 8);
    u32ToBE(powNonce, msg + 12);

    uint8_t hash[32];
    sha256.reset();
    sha256.update(msg, sizeof(msg));
    sha256.finalize(hash, sizeof(hash));

    uint8_t score = leadingZeroBits(hash);

    // Reject immediately if hash has no leading zero bits at all
    if (score == 0) {
      Serial.println(F("# POW_FAIL non_zeroed_hash reset_nonce=0"));
      resetPow();
      return;
    }

    // Only accept a real solution if it meets difficulty
    if (score >= DIFFICULTY_BITS) {
      Serial.print(F("# POW_SUCCESS nonce="));
      Serial.print(powNonce);
      Serial.print(F(" score="));
      Serial.print(score);
      Serial.print(F(" hash="));
      for (int i = 0; i < 32; ++i) {
        if (hash[i] < 0x10) Serial.print('0');
        Serial.print(hash[i], HEX);
      }
      Serial.println();
      resetPow();
      return;
    }

    // Hash had some leading zeros, but not enough for success
    Serial.print(F("# POW_REJECT nonce="));
    Serial.print(powNonce);
    Serial.print(F(" score="));
    Serial.println(score);

    powNonce++;
  }
}
