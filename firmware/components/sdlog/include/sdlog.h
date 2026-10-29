// Logs the RaceBox data messages to a FAT32 microSD card as CSV (same layout as the M5Stack gmeter
// logs: `#` comment lines, a header row, one row per sample). Only the variants built with
// RACEBOX_SDLOG (the C6-GEEK) have a card slot; elsewhere every call below does nothing.
//
// Recording starts by itself at boot if a card is present. No card, a card that cannot be mounted
// (exFAT, ...) or a full / failing card means no recording, and it is never retried. A long press of
// the button stops recording and unmounts the card: once state() is Safe the card may be removed.
#pragma once

#include <stdint.h>

#include "racebox.h"

namespace sdlog {

enum class State : uint8_t {
  Off,     // no card (or no slot on this board)
  Rec,     // recording
  Saving,  // stop requested, flushing; do not remove the card
  Safe,    // stopped and unmounted; the card may be removed
  Failed,  // disk full or write error; recording stopped
};

// Initialises the SPI bus (shared with the LCD, so call it before bridge::frontendStart()), mounts the
// card and starts recording.
void init();
// Queues one data message; never blocks. `micro`: the battery field is a voltage.
void write(const racebox::Data& d, bool micro);
// Stops recording and unmounts the card (returns at once; state() goes Saving, then Safe).
void requestStop();
// From Safe: mounts the card again and records to the next file (log_<session>_02.csv, ...).
void requestStart();
State state();

}  // namespace sdlog
