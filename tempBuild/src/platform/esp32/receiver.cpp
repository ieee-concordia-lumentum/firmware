// ============================================================
// Packet:
//
//   SSS | DATA | ECC | EEE
//
// SSS  = 111
// EEE  = 000
//
// DATA:
//
//   [MIME length]
//   [MIME string]
//   [payload length]
//   [payload]
//
// Reed-Solomon:
//
//   4 ECC symbols
//   GF(256)
//   primitive polynomial 0x11D
//   corrects up to 2 corrupted symbols
// ============================================================

#include <LiquidCrystal.h>

const int DATA_PIN = 27; // D27 input pin on es32

const unsigned long BIT_PERIOD_US = 50000; // MUST MATCH MEGA

const int START_BITS = 3;
const int STOP_BITS  = 3;

// Start = 111
// Stop  = 000
const uint8_t START_BIT = HIGH;
const uint8_t STOP_BIT  = LOW;


// Reed-Solomon config.
// NOTE: everything reed-solomon related was done by ai
const int RS_ECC_SYMBOLS = 4;


// Maximum DATA bytes in one RS block.
//
// 255 total GF(256) symbols
// - 4 ECC symbols
// = 251 data symbols
const int MAX_DATA_BYTES = 251;


// Maximum complete codeword.
const int MAX_CODEWORD_BYTES =
    MAX_DATA_BYTES + RS_ECC_SYMBOLS;


// ============================================================
// LCD
// ============================================================

// RS, E, D4, D5, D6, D7
LiquidCrystal lcd(
    23,
    22,
    21,
    19,
    18,
    13
);


// ============================================================
// GF(256) TABLES
// ============================================================

uint8_t gfExp[512];
uint8_t gfLog[256];


// ============================================================
// INITIALIZE GF(256)
// ============================================================

void initGaloisField()
{
    uint16_t x = 1;

    for (int i = 0; i < 255; i++)
    {
        gfExp[i] = x;
        gfLog[x] = i;

        x <<= 1;

        if (x & 0x100)
        {
            x ^= 0x11D;
        }
    }

    for (int i = 255; i < 512; i++)
    {
        gfExp[i] =
            gfExp[i - 255];
    }
}


// ============================================================
// GF OPERATIONS
// ============================================================

uint8_t gfMultiply(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0)
        return 0;

    return gfExp[
        gfLog[a] + gfLog[b]
    ];
}


uint8_t gfInverse(uint8_t a)
{
    if (a == 0)
        return 0;

    return gfExp[
        255 - gfLog[a]
    ];
}


// ============================================================
// POLYNOMIAL EVALUATION
//
// Polynomial coefficients are stored highest degree first.
// ============================================================

uint8_t polynomialEvaluate(
    const uint8_t *poly,
    int length,
    uint8_t x
)
{
    uint8_t result = poly[0];

    for (int i = 1; i < length; i++)
    {
        result =
            gfMultiply(result, x)
            ^ poly[i];
    }

    return result;
}


// ============================================================
// REED-SOLOMON DECODER
//
// 4 ECC symbols.
// Corrects up to 2 symbol errors.
//
// Returns:
//
//   0 = no errors
//   1 = corrected one symbol
//   2 = corrected two symbols
//  -1 = unable to correct
// ============================================================

int rsDecode(
    uint8_t *codeword,
    int codewordLength
)
{
    if (codewordLength <= RS_ECC_SYMBOLS)
    {
        return -1;
    }


    // --------------------------------------------------------
    // Calculate four syndromes.
    // --------------------------------------------------------

    uint8_t syndrome[4];

    for (int i = 0; i < 4; i++)
    {
        syndrome[i] =
            polynomialEvaluate(
                codeword,
                codewordLength,
                gfExp[i]
            );
    }


    // --------------------------------------------------------
    // No errors.
    // --------------------------------------------------------

    if (syndrome[0] == 0 &&
        syndrome[1] == 0 &&
        syndrome[2] == 0 &&
        syndrome[3] == 0)
    {
        return 0;
    }


    uint8_t S0 = syndrome[0];
    uint8_t S1 = syndrome[1];
    uint8_t S2 = syndrome[2];
    uint8_t S3 = syndrome[3];


    // ========================================================
    // TRY ONE ERROR
    // ========================================================

    if (S0 != 0)
    {
        uint8_t X =
            gfMultiply(
                S1,
                gfInverse(S0)
            );

        uint8_t X2 =
            gfMultiply(X, X);

        uint8_t X3 =
            gfMultiply(X2, X);

        if (gfMultiply(S0, X2) == S2 &&
            gfMultiply(S0, X3) == S3)
        {
            int degree = gfLog[X];

            if (degree < codewordLength)
            {
                int position =
                    codewordLength
                    - 1
                    - degree;

                // Error magnitude.
                codeword[position] ^= S0;


                // Verify correction.
                bool corrected = true;

                for (int i = 0; i < 4; i++)
                {
                    if (
                        polynomialEvaluate(
                            codeword,
                            codewordLength,
                            gfExp[i]
                        ) != 0
                    )
                    {
                        corrected = false;
                    }
                }

                if (corrected)
                {
                    return 1;
                }

                // Restore if verification failed.
                codeword[position] ^= S0;
            }
        }
    }


    // ========================================================
    // TRY TWO ERRORS
    // ========================================================

    uint8_t determinant =
        gfMultiply(S1, S1)
        ^
        gfMultiply(S0, S2);

    if (determinant == 0)
    {
        return -1;
    }


    uint8_t sigma1Numerator =
        gfMultiply(S2, S1)
        ^
        gfMultiply(S3, S0);


    uint8_t sigma2Numerator =
        gfMultiply(S1, S3)
        ^
        gfMultiply(S2, S2);


    uint8_t sigma1 =
        gfMultiply(
            sigma1Numerator,
            gfInverse(determinant)
        );


    uint8_t sigma2 =
        gfMultiply(
            sigma2Numerator,
            gfInverse(determinant)
        );


    // --------------------------------------------------------
    // Find roots of error locator polynomial.
    // --------------------------------------------------------

    int found = 0;

    int position1 = -1;
    int position2 = -1;

    uint8_t X1 = 0;
    uint8_t X2 = 0;


    for (int degree = 0;
         degree < codewordLength;
         degree++)
    {
        uint8_t X =
            gfExp[degree];

        uint8_t inverseX =
            gfInverse(X);

        uint8_t inverseX2 =
            gfMultiply(
                inverseX,
                inverseX
            );

        uint8_t value =
            1
            ^
            gfMultiply(
                sigma1,
                inverseX
            )
            ^
            gfMultiply(
                sigma2,
                inverseX2
            );

        if (value == 0)
        {
            int position =
                codewordLength
                - 1
                - degree;

            if (found == 0)
            {
                position1 = position;
                X1 = X;
            }
            else if (found == 1)
            {
                position2 = position;
                X2 = X;
            }

            found++;
        }
    }


    if (found != 2)
    {
        return -1;
    }


    // ========================================================
    // Calculate error magnitudes.
    // ========================================================

    uint8_t denominator =
        X2 ^ X1;

    if (denominator == 0)
    {
        return -1;
    }


    uint8_t error1Numerator =
        gfMultiply(S0, X2)
        ^
        S1;


    uint8_t error2Numerator =
        S1
        ^
        gfMultiply(S0, X1);


    uint8_t error1 =
        gfMultiply(
            error1Numerator,
            gfInverse(denominator)
        );


    uint8_t error2 =
        gfMultiply(
            error2Numerator,
            gfInverse(denominator)
        );


    // --------------------------------------------------------
    // Correct the two symbols.
    // --------------------------------------------------------

    codeword[position1] ^= error1;
    codeword[position2] ^= error2;


    // --------------------------------------------------------
    // Verify.
    // --------------------------------------------------------

    for (int i = 0; i < 4; i++)
    {
        if (
            polynomialEvaluate(
                codeword,
                codewordLength,
                gfExp[i]
            ) != 0
        )
        {
            return -1;
        }
    }


    return 2;
}


// ============================================================
// TIMED BIT READER
//
// nextSampleTime is the exact center of the next bit.
// dont put serial.println() here cuz that messes up the timings.
// ============================================================

unsigned long nextSampleTime;


// wait until the scheduled sampling point and read one bit.
int readTimedBit()
{
    while ((long)(micros() - nextSampleTime) < 0)
    {
        
    }

    int bit =
        digitalRead(DATA_PIN);

    nextSampleTime +=
        BIT_PERIOD_US;

    return bit ? 1 : 0;
}

bool waitForStart()
{
    // --------------------------------------------------------
    // Require sustained LOW idle.
    // --------------------------------------------------------
    const unsigned long IDLE_THRESHOLD_US = 20UL * BIT_PERIOD_US;

    while (true)
    {
        while (digitalRead(DATA_PIN) == HIGH) {}

        unsigned long lowStart = micros();
        bool sustained = true;

        while ((micros() - lowStart) < IDLE_THRESHOLD_US)
        {
            if (digitalRead(DATA_PIN) == HIGH)
            {
                sustained = false;
                break;
            }
        }
        if (sustained) break;
    }

    while (digitalRead(DATA_PIN) == LOW) {}
    while (digitalRead(DATA_PIN) == HIGH) {}

    // should now be sintting in the low gap. return out now so the readByte() can catch the bits in time.
    return true;
}

// READ BYTE
// MSB first.

uint8_t readByteTimed()
{
    // 1. Wait for the line to be LOW (The previous Stop Bit or Idle state)
    while (digitalRead(DATA_PIN) == HIGH) {
    }

    // 2. Wait for the START BIT (Rising Edge)
    while (digitalRead(DATA_PIN) == LOW) {
    }

    // 3. RESYNCHRONIZE CLOCK! 
    // We are at the start of the Start Bit. 
    // The center of the first Data Bit is exactly 1.5 bit periods away.
    nextSampleTime = micros() + BIT_PERIOD_US + (BIT_PERIOD_US / 2);

    // 4. Read the 8 Data bits
    uint8_t value = 0;
    for (int i = 0; i < 8; i++)
    {
        int bit = readTimedBit();
        value = (value << 1) | bit;
    }

    // 5. Consume the STOP BIT (Wait for its center)
    // We don't necessarily need to read its value, we just need the 
    // timeline to advance past it so we are ready for the next Start Bit.
    int stopBit = readTimedBit();

    return value;
}


// ============================================================
// READ SEPTET LENGTH
//
// Lengths use little-endian 7-bit groups.
//
// MSB = 1:
//     another byte follows.
//
// MSB = 0:
//     final byte.
// ============================================================

int readLengthTimed()
{
    uint16_t value = 0;

    int shift = 0;

    while (true)
    {
        uint8_t byte =
            readByteTimed();

        uint8_t septet =
            byte & 0x7F;

        value |=
            ((uint16_t)septet << shift);


        // Final septet.
        if ((byte & 0x80) == 0)
        {
            return value;
        }


        shift += 7;

        // Maximum supported length:
        // 14 bits.
        if (shift >= 14)
        {
            return -1;
        }
    }
}


// ============================================================
// READ STOP BITS
// ============================================================
//
// IMPORTANT:
//
// This function must be called IMMEDIATELY after the final
// ECC byte is received.
//
// Do not print debug information before calling this.
// ============================================================

bool readStopBits()
{
    for (int i = 0;
         i < STOP_BITS;
         i++)
    {
        int bit =
            readTimedBit();

        if (bit != STOP_BIT)
        {
            return false;
        }
    }

    return true;
}


// ============================================================
// DISPLAY PAYLOAD
// ============================================================

void displayPayload(
    const uint8_t *payload,
    int payloadLength
)
{
    lcd.clear();

    lcd.setCursor(0, 0);
    lcd.print("Payload:");

    lcd.setCursor(0, 1);

    int count =
        payloadLength;

    if (count > 16)
    {
        count = 16;
    }

    for (int i = 0; i < count; i++)
    {
        lcd.print(
            (char)payload[i]
        );
    }
}


// ============================================================
// DEBUG CODEWORD
//
// This function is deliberately called ONLY after all timed
// reception is complete.
//
// Therefore Serial printing cannot disturb the protocol.
// ============================================================

void debugCodeword(
    const uint8_t *codeword,
    int dataLength,
    int codewordLength
)
{
    Serial.println();
    Serial.println("------------------------------");

    Serial.print("dataLength = ");
    Serial.println(dataLength);

    Serial.print("codewordLength = ");
    Serial.println(codewordLength);

    Serial.print("Last bytes read (hex): ");

    int start =
        (codewordLength > 8)
        ? codewordLength - 8
        : 0;

    for (int i = start;
         i < codewordLength;
         i++)
    {
        if (codeword[i] < 0x10)
        {
            Serial.print('0');
        }

        Serial.print(
            codeword[i],
            HEX
        );

        Serial.print(' ');
    }

    Serial.println();

    Serial.println("------------------------------");
}


// ============================================================
// RECEIVE COMPLETE PACKET
// ============================================================

bool receivePacket()
{
    uint8_t data[
        MAX_DATA_BYTES
    ];

    uint8_t codeword[
        MAX_CODEWORD_BYTES
    ];


    // ========================================================
    // WAIT FOR START
    // ========================================================

    waitForStart();


    // ========================================================
    // MIME LENGTH
    // ========================================================

    int mimeLength =
        readLengthTimed();


    if (mimeLength < 0)
    {
        Serial.println(
            "Invalid MIME length."
        );

        return false;
    }


    if (mimeLength > MAX_DATA_BYTES)
    {
        Serial.println(
            "MIME too large."
        );

        return false;
    }


    // ========================================================
    // RECONSTRUCT MIME LENGTH
    // ========================================================

    int dataLength = 0;

    uint16_t tempLength =
        mimeLength;


    do
    {
        uint8_t septet =
            tempLength & 0x7F;

        tempLength >>= 7;

        if (tempLength != 0)
        {
            septet |= 0x80;
        }

        data[dataLength++] =
            septet;

    } while (tempLength != 0);


    // ========================================================
    // MIME STRING
    // ========================================================

    char mime[64];

    if (mimeLength >= 64)
    {
        Serial.println(
            "MIME string too long."
        );

        return false;
    }


    for (int i = 0;
         i < mimeLength;
         i++)
    {
        uint8_t byte =
            readByteTimed();

        data[dataLength++] =
            byte;

        mime[i] =
            (char)byte;
    }

    mime[mimeLength] =
        '\0';


    // ========================================================
    // PAYLOAD LENGTH
    // ========================================================

    int payloadLength =
        readLengthTimed();


    if (payloadLength < 0)
    {
        Serial.println(
            "Invalid payload length."
        );

        return false;
    }


    // Reconstruct exact encoded length bytes.
    tempLength =
        payloadLength;


    do
    {
        uint8_t septet =
            tempLength & 0x7F;

        tempLength >>= 7;

        if (tempLength != 0)
        {
            septet |= 0x80;
        }

        if (dataLength >= MAX_DATA_BYTES)
        {
            return false;
        }

        data[dataLength++] =
            septet;

    } while (tempLength != 0);


    // ========================================================
    // PAYLOAD
    // ========================================================

    if (payloadLength > MAX_DATA_BYTES)
    {
        Serial.println(
            "Payload too large."
        );

        return false;
    }


    uint8_t payload[
        MAX_DATA_BYTES
    ];


    for (int i = 0;
         i < payloadLength;
         i++)
    {
        uint8_t byte =
            readByteTimed();

        payload[i] =
            byte;

        if (dataLength >= MAX_DATA_BYTES)
        {
            return false;
        }

        data[dataLength++] =
            byte;
    }


    // ========================================================
    // DATA LENGTH NOW KNOWN
    // ========================================================

    if (dataLength <= 0 ||
        dataLength > MAX_DATA_BYTES)
    {
        return false;
    }


    // ========================================================
    // COPY DATA INTO CODEWORD
    // ========================================================

    for (int i = 0;
         i < dataLength;
         i++)
    {
        codeword[i] =
            data[i];
    }


    // ========================================================
    // READ ECC
    //
    // IMPORTANT:
    //
    // There is NO Serial output between the last ECC byte
    // and the stop-bit sampling.
    // ========================================================

    for (int i = 0;
         i < RS_ECC_SYMBOLS;
         i++)
    {
        codeword[
            dataLength + i
        ] =
            readByteTimed();
    }


    int codewordLength =
        dataLength +
        RS_ECC_SYMBOLS;


    // ========================================================
    // STOP BITS
    //
    // MUST happen immediately after ECC.
    // ========================================================

    bool validStopBits =
        readStopBits();


    // ========================================================
    // ONLY NOW IS IT SAFE TO PRINT DEBUG INFORMATION.
    // ========================================================

    debugCodeword(
        codeword,
        dataLength,
        codewordLength
    );


    if (!validStopBits)
    {
        Serial.println(
            "Invalid stop bits."
        );

        return false;
    }


    Serial.println(
        "Stop bits: valid"
    );


    // ========================================================
    // REED-SOLOMON DECODE
    // ========================================================

    int corrected =
        rsDecode(
            codeword,
            codewordLength
        );


    if (corrected < 0)
    {
        Serial.println(
            "RS ERROR: unable to correct packet."
        );

        lcd.clear();

        lcd.setCursor(0, 0);
        lcd.print("RS ERROR");

        return false;
    }


    // ========================================================
    // EXTRACT PAYLOAD FROM CORRECTED CODEWORD
    // ========================================================

    int index = 0;


    // --------------------------------------------------------
    // MIME LENGTH
    // --------------------------------------------------------

    int correctedMimeLength = 0;

    int shift = 0;

    while (true)
    {
        uint8_t byte =
            codeword[index++];

        correctedMimeLength |=
            (byte & 0x7F)
            << shift;

        if ((byte & 0x80) == 0)
        {
            break;
        }

        shift += 7;
    }


    // --------------------------------------------------------
    // Skip MIME.
    // --------------------------------------------------------

    index +=
        correctedMimeLength;


    // --------------------------------------------------------
    // Corrected payload length.
    // --------------------------------------------------------

    int correctedPayloadLength = 0;

    shift = 0;

    while (true)
    {
        uint8_t byte =
            codeword[index++];

        correctedPayloadLength |=
            (byte & 0x7F)
            << shift;

        if ((byte & 0x80) == 0)
        {
            break;
        }

        shift += 7;
    }


    // ========================================================
    // DISPLAY PAYLOAD
    // ========================================================

    displayPayload(
        &codeword[index],
        correctedPayloadLength
    );


    // ========================================================
    // DEBUG OUTPUT
    // ========================================================

    Serial.println();
    Serial.println(
        "=========================="
    );

    Serial.println(
        "PACKET RECEIVED"
    );

    Serial.print(
        "MIME: "
    );

    Serial.println(
        mime
    );

    Serial.print(
        "Payload: "
    );

    for (
        int i = 0;
        i < correctedPayloadLength;
        i++
    )
    {
        Serial.print(
            (char)codeword[
                index + i
            ]
        );
    }

    Serial.println();

    Serial.print(
        "Data bytes: "
    );

    Serial.println(
        dataLength
    );

    Serial.print(
        "ECC bytes: "
    );

    Serial.println(
        RS_ECC_SYMBOLS
    );

    Serial.print(
        "Codeword bytes: "
    );

    Serial.println(
        codewordLength
    );

    Serial.print(
        "RS result: "
    );

    if (corrected == 0)
    {
        Serial.println(
            "no errors"
        );
    }
    else if (corrected == 1)
    {
        Serial.println(
            "corrected 1 symbol"
        );
    }
    else if (corrected == 2)
    {
        Serial.println(
            "corrected 2 symbols"
        );
    }

    Serial.println(
        "=========================="
    );


    return true;
}


// ============================================================
// SETUP
// ============================================================

void setup()
{
    pinMode(
        DATA_PIN,
        INPUT
    );


    Serial.begin(115200);


    initGaloisField();


    lcd.begin(16, 2);

    lcd.clear();

    lcd.setCursor(0, 0);

    lcd.print(
        "Waiting..."
    );


    Serial.println();

    Serial.println(
        "Custom protocol receiver ready."
    );

    Serial.print(
        "BIT_PERIOD_US = "
    );

    Serial.println(
        BIT_PERIOD_US
    );
}

void loop()
{
    receivePacket();
}

