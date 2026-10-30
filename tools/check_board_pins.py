"""Compile-check pin conflicts for the current wiring and optional battery-divider wiring."""

import argparse
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include "board_pins.h"
constexpr int pins[] = {PIN_LCD_CS, PIN_LCD_DC, PIN_TP_INT, PIN_TP_RST, PIN_I2C_SDA,
  PIN_I2C_SCL, PIN_LCD_RST, PIN_LCD_BL, PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_BATTERY};
constexpr bool noConflicts() {
  for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i)
    for (unsigned j = i + 1; j < sizeof(pins) / sizeof(pins[0]); ++j)
      if (pins[i] >= 0 && pins[i] == pins[j] && !(i == 3 && j == 6)) return false;
  return true;
}
static_assert(noConflicts(), "Unintended GPIO collision");
static_assert(PIN_SPI_MISO == 8 && PIN_SD_CS == 21, "Sense SD wiring changed");
#ifdef PERCH_BATTERY_DIVIDER
static_assert(PIN_BATTERY == 4 && PIN_TP_RST == PIN_LCD_RST, "Battery requires shared reset");
#else
static_assert(PIN_BATTERY == -1 && PIN_TP_RST == 4, "Default wiring must remain unchanged");
#endif
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler")
    args = parser.parse_args()
    bundled = Path.home() / ".platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-g++.exe"
    compiler = args.compiler or (str(bundled) if bundled.exists() else shutil.which("g++"))
    if not compiler:
        raise SystemExit("Install PlatformIO or provide --compiler")
    for name, flags in (("current wiring", []), ("battery divider", ["-DPERCH_BATTERY_DIVIDER"])):
        subprocess.run([compiler, "-std=c++17", "-fsyntax-only", "-x", "c++", "-I", str(ROOT / "include"),
                        *flags, "-"], input=SOURCE, text=True, check=True)
        print("PASS", name)


if __name__ == "__main__":
    main()
