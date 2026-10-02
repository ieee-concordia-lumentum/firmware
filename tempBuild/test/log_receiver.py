"""Save received words, separated by spaces, to test/log.txt."""
import argparse
import time
from pathlib import Path

#ignore the import warning :]
import serial 

parser = argparse.ArgumentParser()
parser.add_argument("--port", required=True, help="Receiver port, e.g. COM3")
parser.add_argument("--output", type=Path,
                    default=Path(__file__).resolve().parent / "log.txt")
parser.add_argument("--duration", type=float, default=930,
                    help="Capture seconds after the first received message")
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)

started = None
firstWord = True

# Close the receiver serial monitor before opening this logger.
with serial.Serial(args.port, 115200, timeout=0.25) as port:
    with args.output.open("w", encoding="utf-8") as log:
        print(f"Saving words to {args.output}")
        print("Enter s in the sender serial monitor to start.")
        try:
            while started is None or time.monotonic() - started < args.duration:
                raw = port.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").strip()
                if not line.startswith("DATA,"):
                    continue
                if started is None:
                    started = time.monotonic()
                fields = line.split(",")
                if len(fields) != 9 or fields[6] != "OK":
                    continue
                word = fields[3]
                if not word.isascii() or not word.isalpha():
                    continue
                separator = "" if firstWord else " "
                log.write(separator + word)
                log.flush()
                print(separator + word, end="", flush=True)
                firstWord = False
        except KeyboardInterrupt:
            pass
        print()
