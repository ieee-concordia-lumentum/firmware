Current terminal usage (desktop packet/ECC, MCU bit timing):
- src/main.cpp has moved to src/MCU/main.cpp.
- src/Desktop/main.cpp contains toTransmit and toReceive, with no GUI.
- src/protocol.c keeps the original Reed-Solomon helpers and packet format.
  DESKTOP builds encode/decode; firmware builds only buffering and timed bits.
- platformio.ini excludes Desktop/ and the unrelated files.c from MCU builds.
- A physical frame always contains 609 encoded bytes after the existing
  AAAA/D3 synchronization. Unused packet space is zero padded. Both boards
  must use the updated firmware; it is not compatible with old short frames.
- CRC uses the existing nine-byte trailer, leaving 247 bytes for each message.
- Sender GPIO 18, receiver GPIO 19, serial 115200 baud, optical 16 kbps.

Build from tempBuild (MinGW g++ on PATH; create .pio first if it does not exist):
    g++ -std=c++11 -DDESKTOP -Iinclude src/Desktop/main.cpp src/protocol.c -o .pio/desktop.exe
    pio run
Flash the correct role to each board using PlatformIO.

Start the receiver terminal FIRST:
    .\.pio\desktop.exe toReceive COM4
Then on the sender computer (replace ports with your actual ports):
    .\.pio\desktop.exe toTransmit COM3 "Hello"
Or omit "Hello" to enter one message per line. Ctrl+Z then Enter finishes
interactive sending; Ctrl+C stops receiving. Close other serial monitors first.

Sender completion means local laser output finished, not remote delivery.
There are no remote acknowledgments/retries or large-file splitting. The sender
waits 200 ms between sends to allow receiver serial forwarding; this is pacing,
not a delivery guarantee. Initial sampling alignment is unchanged; hardware
clock drift, ISR timing, and optical quality still require board testing.
The receiver discards packets that fail ECC/markers/CRC and prints valid text.
Desktop serial errors/timeouts require reconnecting and retrying the transfer.

The existing ECC test now also checks packet round trips through the actual
MCU bit functions for lengths 1..256 with zero, one, and two erroneous symbols
per codeword. Its old uint8_t mask
loops were changed to unsigned counters so the exhaustive test terminates.

Historical test/logger notes (retained for reference; the old word logger is
not compatible with the new RX encoded-byte records):

Firmware:
- 16 kbps, existing 56-bit RS header and 14-byte RS data packets.
- Sender starts only when you enter s in its USB serial monitor.
- Every message includes a sequence number and a CRC32.
- Send as soon as the prior transmission finishes, with a 5 ms idle gap.
- CRC is application test data, not a change to the protocol framing.
- Receiver sampling still uses the current initial-preamble timing.

Build and flash both board environments.
In the project directory, run:
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" .\test\log_receiver.py --port COM3
Replace COM3 with the receiver port.

All messages must pass CRC before being treated as valid.
Sequence gaps include both optical loss and any serial logging drops.
A 5 ms gap is a conservative starting point, not a measured maximum cadence.

ECC repair test (PC only, no flashing or wiring required):
From tempBuild in PowerShell, run:
    .\test\run_ecc_test.ps1
Requires GCC on PATH (available on this computer).
It uses the actual ECC code from src/protocol.c, not a copied algorithm.
For four example payloads, it tests the 7-byte header and 10-byte data block:
- An unchanged block must return 0 and keep all bytes unchanged.
- Every single-byte corruption must return 1 and restore every byte.
- Every two-byte corruption must return 2 and restore every byte.
All positions and all nonzero XOR masks are tried, including parity bytes.
Expect PASS with zero failures; a failing run prints the first ten bad cases.
The script builds a temporary executable and removes it after the run.
This does not test the optical link, receiver sampling, or complete frames.
More than two damaged bytes may be rejected or incorrectly repaired; use
message CRC checking on the receiver to detect additional corruption.
