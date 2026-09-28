#ifndef OTA_IMAGE_H
#define OTA_IMAGE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Is this byte stream a Figo firmware image for THIS chip? Answered while the
// bytes stream past, before they are trusted - the OTA upload never holds more
// than one chunk in RAM.
//
// Dependency-free ON PURPOSE - <stddef.h>, <stdint.h> and <string.h>, nothing
// else - so it can be compiled and unit-tested on a host with a plain g++. The
// failure mode is silent (a wrong file is accepted, or the right one refused,
// and nothing says why), which is exactly the shape of bug worth pinning down
// off-device. Don't add includes. Same contract as SleepWindow.h and
// SearchFilter.h.
//
// What it checks, in the order the bytes arrive:
//   - byte 0 is 0xE9, the ESP image magic
//   - chip_id (header bytes 12-13, u16 LE) is 0x000D, the ESP32-C6
//   - it is not the MERGED image: the Arduino IDE writes Figo.ino.merged.bin
//     right beside Figo.ino.bin, and the merged one starts with the bootloader
//     and carries the partition table at 0x8000 (magic AA 50). It also starts
//     with 0xE9 for the right chip, so nothing earlier catches it.
//   - a "FIGO-FW:<version>\0" marker exists somewhere, so a stock C6 image is
//     refused. Version is 1..15 printable characters.
//   - the total fits the app slot.
//
// THE MARKER MUST NOT APPEAR HERE AS A STRING. This header is compiled into
// the firmware, and the checker is scanning firmware images: a plain
// "FIGO-FW:" literal in the pattern would put a second copy of the marker in
// every build, and a scan could lock onto it instead of the real
// FIGO_FW_ID, reporting whatever bytes follow as the version. So the pattern is
// stored shifted by one (each byte + 1) and compared shifted back. Keep the
// same restraint in any message text: never write the marker with its colon.

enum OtaImageResult : uint8_t {
  OTA_IMG_OK = 0,   // nothing wrong so far (feed) / a complete good image (finish)
  OTA_IMG_TRUNCATED,    // ended before the image header was complete
  OTA_IMG_BAD_MAGIC,    // byte 0 is not 0xE9
  OTA_IMG_WRONG_CHIP,   // built for a different ESP32 family
  OTA_IMG_MERGED_BIN,   // Figo.ino.merged.bin, not Figo.ino.bin
  OTA_IMG_NO_MARKER,    // a valid ESP32-C6 image, but not a Figo one
  OTA_IMG_BAD_VERSION,  // marker found, version empty / unterminated / over 15 / unprintable
  OTA_IMG_TOO_BIG       // larger than the app slot
};

#define OTA_IMAGE_VERSION_MAX 15

class OtaImageCheck {
 public:
  OtaImageCheck() { begin(0); }

  // Reset for a new image. slotBytes is the largest image the slot can hold.
  void begin(uint32_t slotBytes) {
    slot_ = slotBytes;
    pos_ = 0;
    result_ = OTA_IMG_OK;
    chipLo_ = 0;
    tableMagic_ = false;
    scan_ = 0;
    verLen_ = 0;
    state_ = SCANNING;
    ver_[0] = 0;
  }

  // Feed the next chunk. Returns OTA_IMG_OK while the image is still plausible,
  // else the first problem found; a failure is sticky. Chunk boundaries can
  // fall anywhere, including inside the marker.
  OtaImageResult feed(const uint8_t *data, size_t len) {
    if (result_ != OTA_IMG_OK || len == 0) return result_;
    if ((uint64_t)pos_ + len > slot_) {
      result_ = OTA_IMG_TOO_BIG;
      return result_;
    }
    for (size_t i = 0; i < len && result_ == OTA_IMG_OK; i++) step(data[i]);
    return result_;
  }

  // Call once after the last byte.
  OtaImageResult finish() {
    if (result_ != OTA_IMG_OK) return result_;
    if (pos_ < HEADER_BYTES) return result_ = OTA_IMG_TRUNCATED;
    if (state_ == SCANNING) return result_ = OTA_IMG_NO_MARKER;
    if (state_ == IN_VERSION) return result_ = OTA_IMG_BAD_VERSION;  // ended before the NUL
    return result_;
  }

  // Valid only after a feed/finish that returned OTA_IMG_OK.
  const char *version() const { return ver_; }
  uint32_t bytesSeen() const { return pos_; }

 private:
  enum State : uint8_t { SCANNING, IN_VERSION, FOUND };

  static const uint32_t HEADER_BYTES = 14;   // through chip_id
  static const uint32_t TABLE_OFFSET = 0x8000;

  // "FIGO-FW:" with every byte + 1. See the block comment above.
  static char pat(uint8_t i) {
    static const char P[8] = {'G', 'J', 'H', 'P', '.', 'G', 'X', ';'};
    return P[i];
  }
  // KMP failure table for that pattern (the 'F' at index 5 restarts a match).
  static uint8_t fail(uint8_t i) {
    static const uint8_t F[8] = {0, 0, 0, 0, 0, 1, 0, 0};
    return F[i];
  }

  void step(uint8_t b) {
    if (pos_ == 0 && b != 0xE9) { result_ = OTA_IMG_BAD_MAGIC; return; }
    if (pos_ == 12) chipLo_ = b;
    if (pos_ == 13 && (uint16_t)(chipLo_ | (b << 8)) != 0x000D) {
      result_ = OTA_IMG_WRONG_CHIP;
      return;
    }
    if (pos_ == TABLE_OFFSET) tableMagic_ = (b == 0xAA);
    if (pos_ == TABLE_OFFSET + 1 && tableMagic_ && b == 0x50) {
      result_ = OTA_IMG_MERGED_BIN;
      return;
    }
    pos_++;

    if (state_ == SCANNING) {
      while (scan_ > 0 && (char)(uint8_t)(b + 1) != pat(scan_)) scan_ = fail(scan_ - 1);
      if ((char)(uint8_t)(b + 1) == pat(scan_)) scan_++;
      if (scan_ == 8) state_ = IN_VERSION;
    } else if (state_ == IN_VERSION) {
      if (b == 0) {
        if (verLen_ == 0) result_ = OTA_IMG_BAD_VERSION;
        else state_ = FOUND;  // keep counting bytes for the size and merged checks
      } else if (b >= 0x20 && b <= 0x7E && verLen_ < OTA_IMAGE_VERSION_MAX) {
        ver_[verLen_++] = (char)b;
        ver_[verLen_] = 0;
      } else {
        result_ = OTA_IMG_BAD_VERSION;
      }
    }
  }

  uint32_t slot_;
  uint32_t pos_;
  OtaImageResult result_;
  uint8_t chipLo_;
  bool tableMagic_;
  uint8_t scan_;    // bytes of the marker matched so far
  uint8_t verLen_;
  State state_;
  char ver_[OTA_IMAGE_VERSION_MAX + 1];
};

#endif  // OTA_IMAGE_H
