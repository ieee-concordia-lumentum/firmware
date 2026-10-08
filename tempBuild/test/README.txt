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
