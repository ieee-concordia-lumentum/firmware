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
