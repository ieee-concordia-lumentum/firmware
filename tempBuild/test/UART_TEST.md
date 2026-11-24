# Hardware UART mode

This prototype replaces software bit timing with UART1 at 460800 baud, 8N1.
The computer-to-board USB connection remains 460800 baud. These are separate links.

Keep sender GPIO18 connected to receiver GPIO19 and connect the board grounds.
The link remains one-way. Use both UART firmware roles together; timer and UART modes
are not wire-compatible. Unused receiver TX and sender RX pins need no connection.

Stop existing desktop programs and serial monitors, then flash:

```powershell
pio run -e esp32_uart_receiver -t upload --upload-port COM7
pio run -e esp32_uart_sender -t upload --upload-port COM3
```

Use the desktop program. Start the receiver first:

```powershell
.\.pio\desktop.exe toReceive COM7
```

In another terminal:

```powershell
.\.pio\desktop.exe toTransmit COM3
```

Move existing destination files out of toReceive before retrying. Start with a small
file, then repeat the same image several times using distinct destination names or
moving the previous received copy. Confirm Saved ... CRC OK each time.

The wrapper uses COBS delimiters around a sequence number, the unchanged ECC frame,
and a CRC32. CRC-damaged but correctly framed records still reach desktop ECC and
file CRC validation. Invalid framing is discarded and resynchronizes at a delimiter.
No receiver-to-sender acknowledgment has been introduced. TX_DONE is emitted only
after the hardware UART finishes the last stop bit. The sender loads the next USB frame while UART sends the current one.
The verified setup uses 512-byte messages and a 20 ms gap between UART frames.

Host regression tests:

```powershell
.\test\run_uart_link_test.ps1
.\test\run_ecc_test.ps1
```

Fallback to the previous timer mode by flashing both original environments:

```powershell
pio run -e esp32_receiver -t upload --upload-port COM7
pio run -e esp32_sender -t upload --upload-port COM3
```

The original timer environments remain the default. A source snapshot made before
this change is stored in .pio/baselines/ (local, not a Git commit). The new mode must
pass physical transfer testing before any baud increase or optical testing.

The 115200-baud wired baseline completed 581470 bytes in 172.2 seconds with CRC OK.
The 230400-baud wired test completed the image in 108.5 seconds with CRC OK.
The 460800-baud wired test completed the image in 77.0 seconds with CRC OK.
The 921600-baud test lost frames. At 460800 baud with pipelining, the same image
completed in 68.8 seconds with CRC OK.
