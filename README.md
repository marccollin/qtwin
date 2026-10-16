# QTwin

Qt 6 desktop app that downloads dive logs from a Bluedge D-Twin dive computer over Bluetooth (RFCOMM) and exports them as UDDF 3.2. It replaces the original D-Twin app.

## Build

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/qtwin
```

Requirements: g++, cmake, ninja, Qt 6 (Core, Widgets, Bluetooth, Test).

### Windows (experimental, not device-verified)

The Windows build uses a separate `QBluetoothSocket` adapter in
`src/qtrfcommsocket.cpp`, connecting directly to the selected RFCOMM channel.
Linux continues to use `src/rfcommsocket.cpp`; the experimental UUID probe is
not used by the main application.

Install Visual Studio's C++ build tools, CMake, and a matching Qt 6 MSVC kit
including Qt Bluetooth. From a developer terminal with that Qt kit on PATH:

```bat
cmake -S . -B build-win
cmake --build build-win --config Release
ctest --test-dir build-win -C Release --output-on-failure
windeployqt build-win\Release\qtwin.exe
```

Pair D-Twin in Windows Bluetooth settings, enable PC transfer mode, then run
`build-win\Release\qtwin.exe`. The build, discovery, fixed-channel connection,
downloads, cancellation and reconnection still require verification on Windows
with the device. Do not assume the successful Linux tests validate Windows
Bluetooth behaviour.

## Usage

1. Pair the dive computer with the PC and enable PC connection on it.
2. Click **Search**, then select the `D-Twin` device. QTwin sets the RFCOMM channel automatically: 5 if the name contains `:`, otherwise 1.
3. Click **Load dive list**, then download the selected dives or all dives.

QTwin saves `<output>/DiveN_yyyy_MM_dd.uddf` and the raw `.txt` response. If parsing fails, it keeps the raw data as `Dive<id>_unparsed.txt`.

## Protocol (confirmed with a btmon capture of the original app)

### Experimental Qt Bluetooth connection probe

With the computer paired and waiting for a PC connection, close other applications
using it and run:

```sh
./build/qtwin-bt-probe 00:13:43:04:58:5C
```

This separate console diagnostic connects using Qt's `QBluetoothSocket` and the
Serial Port service UUID, rather than a fixed RFCOMM channel. It requests the dive
count with `GTDL`, then sends `STOP` and waits for `OK`. Connection and response
timeouts and backend errors are printed explicitly. It does not download dives or
change the main application's native Linux transport. Actual device communication
must succeed before considering this connection method supported on a platform.

To investigate connection-startup timing, an optional second argument delays
`GTDL` after connection (0-10000 milliseconds, default 0):

```sh
./build/qtwin-bt-probe 00:13:43:04:58:5C 3500
```

This is a diagnostic experiment, not a confirmed device timing requirement.

The protocol uses plain ASCII commands over RFCOMM. Responses have no terminator, and each record type has a fixed length. The computer answers in about 50–200 ms.

| Command | Response |
|---|---|
| `GTDL` | `DVTOT00217`: number of dives |
| `GTNX` | Next list entry, or `ENOD` at the end |
| `GTDV` | `DVNUM` |
| `DV<id5>` | `DVSTART…` header (60 chars) |
| `GTNR` | `DVLOG…` (21 chars), `DVSSW…` (14 chars, event), `DVGSW…` (16 chars, gas switch), then `END…` (16 chars) |
| `STOP` | `OK` |

List entry `DV0029320261010140617016800300210012` contains:

- dive id `00293`
- `yyyyMMddHHmmss` start time
- duration in units of 10 s (`0168`, 28 min)
- sample interval `003` in seconds
- max depth `0021` in whole meters
- average depth `0012`

`DVLOG0105901749446320` contains:

- time `01059` in seconds
- depth `0174` in tenths
- temperature in hundredths of °C, **without zero padding** (`944` = 9.44 °C)
- an unidentified field, which shifts left when the temperature has fewer digits

## Fixes over the original

- Bluetooth I/O is asynchronous and framed by record length. There are no 2 s sleeps per entry, and the UI no longer freezes.
- Several records received in one read are split and handled one at a time. If the computer doesn't answer `GTDL`, `GTNX` or `GTNR` within 3 s, the command is sent again, up to 3 times. A record repeated because of a retry is ignored.
- Temperatures below 10 °C are read correctly. The original app read a fixed 3-digit slice, so 9.44 °C became 94 °C.
- UDDF output is valid and contains no fabricated tank or air-temperature data.

## Units

The computer always sends metric data (meters, °C), even when its display is set to imperial. This was confirmed with dive 280, an imperial-mode trimix dive to 110.8 m. The original app's "imperial temperature bug" was actually the below-10 °C parsing bug.

## Gas switches

For a dive with exactly one mix, QTwin writes a `tankdata` section linking that mix to the dive for import into Subsurface. Tank volume and pressures are omitted because the device does not provide them; set those values in Subsurface if needed. Multi-mix dives retain their sample-level gas switches without adding tank data.

`DVGSW00040050000` (dive 267, rebreather bailout to EAN50): `[11:13]` O2 %, `[13:15]` He %. It has no timestamp and applies at the preceding `DVLOG` sample, as in the original app's UDDF.

## Unverified assumptions

- `DVSSW` records (e.g. `DVSSW00022-024`, seen on rebreather dives) are ignored; their meaning is unknown (possibly a setpoint switch).

To diagnose a problem, enable **Show Bluetooth traffic** and keep the raw `.txt` files.
