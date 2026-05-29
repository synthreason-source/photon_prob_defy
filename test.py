import serial
import time
import csv
from hashlib import sha256

# -------------------- CONFIG --------------------
SERIAL_PORT = "COM5"          # <-- change to your port, e.g. "/dev/ttyACM0"
BAUD_RATE   = 115200
CSV_LOG     = "vl53_listener_log.csv"

# PoW difficulty (leading‑zero bits) – must match what you want to check on the host
DIFFICULTY_BITS = 16
MAX_TRIALS_PER_SEED = 5_000_000   # safety cap

# -------------------- HELPERS --------------------
def leading_zero_bits(digest: bytes) -> int:
    """Count leading zero bits in a SHA‑256 digest."""
    bits = 0
    for b in digest:
        for bit in range(7, -1, -1):
            if (b >> bit) & 1:
                return bits
            bits += 1
    return bits  # all‑zero case

def mine_for_seed(seed: int, difficulty_bits: int) -> dict:
    """
    Host‑side PoW: try nonce = seed ^ counter until
    SHA256("flatline" || seed || nonce) has >= difficulty_bits leading zero bits.
    Returns a dict with metrics.
    """
    prefix = b"flatline"
    counter = 0
    best_score = 0
    best_nonce = None
    best_digest = None

    t0 = time.perf_counter()
    for _ in range(MAX_TRIALS_PER_SEED):
        nonce = seed ^ counter
        counter += 1

        m = prefix + seed.to_bytes(4, "big") + nonce.to_bytes(4, "big")
        digest = sha256(m).digest()
        score = leading_zero_bits(digest)

        if score > best_score:
            best_score = score
            best_nonce = nonce
            best_digest = digest

        if score >= difficulty_bits:
            break

    t1 = time.perf_counter()
    elapsed = t1 - t0
    if elapsed == 0:
        elapsed = 1e-9
    hps = (counter) / elapsed

    return {
        "trials": counter,
        "elapsed_s": elapsed,
        "hashes_per_s": hps,
        "best_nonce": best_nonce,
        "best_score": best_score,
        "best_digest_hex": best_digest.hex() if best_digest else None,
    }

# -------------------- MAIN LOOP --------------------
def main():
    print(f"Opening {SERIAL_PORT} @ {BAUD_RATE}…")
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1.0)
    time.sleep(2.0)          # let Arduino reset
    ser.reset_input_buffer()

    # Prepare CSV log
    csv_file = open(CSV_LOG, "w", newline="")
    writer = csv.writer(csv_file)
    writer.writerow([
        "t_ms","sample_num","dist_mm","flatline","stable_pairs","seed",
        "arduino_active","arduino_best_nonce","arduino_best_score",
        "host_difficulty_bits","host_trials","host_elapsed_s",
        "host_hashes_per_s","host_best_nonce","host_best_score","host_best_digest_hex"
    ])
    csv_file.flush()

    print("Listening for VL53L1X CSV from Arduino…")

    try:
        while True:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="ignore").strip()
            if not line or line.startswith("#"):
                continue

            parts = line.split(",")
            # Expect at least the first 6 fields; the rest may be missing
            if len(parts) < 6:
                print(f"Skipping too‑short line: {line}")
                continue

            # Parse what we have, fill missing with zeros
            try:
                t_ms         = int(parts[0])
                sample_num   = int(parts[1])
                dist_mm      = int(parts[2])
                flatline     = int(parts[3]) if len(parts) > 3 else 0
                stable_pairs = int(parts[4]) if len(parts) > 4 else 0
                seed         = int(parts[5]) if len(parts) > 5 else 0

                ar_active    = int(parts[6]) if len(parts) > 6 else 0
                ar_best_nonce= int(parts[7]) if len(parts) > 7 else 0
                ar_best_score= int(parts[8]) if len(parts) > 8 else 0
            except ValueError:
                print(f"Parse error on line: {line}")
                continue

            # Echo what we got (helpful for debugging)
            print(f"[SER] t={t_ms}ms s={sample_num} d={dist_mm}mm "
                  f"flat={flatline} pairs={stable_pairs} seed={seed} "
                  f"act={ar_active} bn={ar_best_nonce} bs={ar_best_score}")

            # ---------- Host‑side PoW when a flatline appears ----------
            host_result = {
                "difficulty_bits": DIFFICULTY_BITS,
                "trials": None,
                "elapsed_s": None,
                "hashes_per_s": None,
                "best_nonce": None,
                "best_score": None,
                "best_digest_hex": None,
            }

            if flatline == 1 and seed != 0:
                print(f"[*] Flatline @ t={t_ms}ms, seed={seed} → starting host PoW "
                      f"(difficulty={DIFFICULTY_BITS} bits)…")
                host_result = mine_for_seed(seed, DIFFICULTY_BITS)
                print(f"[POW] done → trials={host_result['trials']:,} "
                      f"elapsed={host_result['elapsed_s']:.3f}s "
                      f"H/s={host_result['hashes_per_s']:,.0f} "
                      f"best_score={host_result['best_score']} "
                      f"best_nonce={host_result['best_nonce']}")

            # ---------- Write to CSV ----------
            writer.writerow([
                t_ms, sample_num, dist_mm,
                flatline, stable_pairs, seed,
                ar_active, ar_best_nonce, ar_best_score,
                host_result["difficulty_bits"],
                host_result["trials"],
                host_result["elapsed_s"],
                host_result["hashes_per_s"],
                host_result["best_nonce"],
                host_result["best_score"],
                host_result["best_digest_hex"]
            ])
            csv_file.flush()

    except KeyboardInterrupt:
        print("\nStopping…")
    finally:
        csv_file.close()
        ser.close()

if __name__ == "__main__":
    main()