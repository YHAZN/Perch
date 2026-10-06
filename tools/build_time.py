# PlatformIO pre-build script: stamps the PC's current time into include/build_time.h
# so a freshly flashed board starts with the right clock even with no Wi-Fi.
Import("env")
import pathlib
import time

header = pathlib.Path(env.subst("$PROJECT_DIR")) / "include" / "build_time.h"
header.write_text(f"#pragma once\n#define PERCH_BUILD_EPOCH {int(time.time())}UL\n")
