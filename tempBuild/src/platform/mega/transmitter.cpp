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


// GPIO used to transmit the bits.
const int DATA_PIN = 2;

unsigned long nextTxTime;
const unsigned long BIT_PERIOD_US = 50000; // 1 ms / bit. ie 1000 bits/s . BOTH MEGA AND ESP32 NEEDS THE SAME VALUE


const int START_BITS = 3;
const int STOP_BITS  = 3;

// Start = 111
// Stop  = 000
const uint8_t START_BIT = HIGH;
const uint8_t STOP_BIT  = LOW;


// Reed-Solomon configuration.
//
// 4 parity symbols means:
//
//     minimum distance = 5
//
// and therefore up to 2 symbol errors can be corrected.
const int RS_ECC_SYMBOLS = 4;


// MIME type that will be transmitted.
const char MIME_TYPE[] = "text/plain";


// Maximum DATA bytes for one RS block.
//
// 255 total GF(256) symbols
// - 4 ECC symbols
// = 251 data symbols
const int MAX_DATA_BYTES = 251;


// ============================================================
// GF(256) TABLES
// ============================================================

uint8_t gfExp[512];
uint8_t gfLog[256];


// Generate GF(256) using:
//
// primitive polynomial = 0x11D
// primitive element     = 2
//
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

    // Duplicate the exponential table so that
    // exponent arithmetic doesn't need modulo in
    // many places.
    for (int i = 255; i < 512; i++)
    {
        gfExp[i] = gfExp[i - 255];
    }
}


// ============================================================
// GALOIS FIELD OPERATIONS
// ============================================================

uint8_t gfMultiply(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0)
        return 0;

    return gfExp[gfLog[a] + gfLog[b]];
}


uint8_t gfInverse(uint8_t a)
{
    if (a == 0)
        return 0;

    return gfExp[255 - gfLog[a]];
}


// ============================================================
// REED-SOLOMON
// ============================================================
//
// Generator polynomial for 4 ECC symbols:
//
//     g(x) = (x - 1)
//          * (x - α)
//          * (x - α²)
//          * (x - α³)
//
// with GF(256), primitive polynomial 0x11D.
//
// The resulting coefficients are:
//
//     [1, 15, 54, 120, 64]
//
// ============================================================

const uint8_t RS_GENERATOR[5] =
{
    1,
    15,
    54,
    120,
    64
};


// ============================================================
// RS ENCODER
//
// Input:
//
//     data[0 ... dataLength-1]
//
// Output:
//
//     codeword[0 ... dataLength+3]
//
// The first dataLength bytes remain unchanged.
// The final 4 bytes are the ECC.
// ============================================================

void rsEncode(
    const uint8_t *data,
    int dataLength,
    uint8_t *codeword
)
{
    int totalLength = dataLength + RS_ECC_SYMBOLS;

    // Copy data.
    for (int i = 0; i < dataLength; i++)
    {
        codeword[i] = data[i];
    }

    // Initialize ECC area to zero.
    for (int i = dataLength; i < totalLength; i++)
    {
        codeword[i] = 0;
    }


    // Polynomial division.
    for (int i = 0; i < dataLength; i++)
    {
        uint8_t coefficient = codeword[i];

        if (coefficient == 0)
            continue;

        for (int j = 1; j <= RS_ECC_SYMBOLS; j++)
        {
            codeword[i + j] ^=
                gfMultiply(RS_GENERATOR[j], coefficient);
        }
    }


    // Polynomial division modified the data portion.
    // Restore the original data.
    for (int i = 0; i < dataLength; i++)
    {
        codeword[i] = data[i];
    }
}


// ============================================================
// PHYSICAL BIT TRANSMISSION
// ============================================================

void sendBit(uint8_t bit)
{
    // Wait for the exact absolute time
    while ((long)(micros() - nextTxTime) < 0)
    {
        // Busy wait
    }

    // Output the bit
    digitalWrite(DATA_PIN, bit ? HIGH : LOW);

    // Schedule the NEXT bit exactly 50ms later
    nextTxTime += BIT_PERIOD_US;
}


// msb first
void sendByte(uint8_t value)
{
    // 1. START BIT (HIGH) forces a rising edge
    sendBit(HIGH);

    // 2. DATA BITS (8 bits)
    for (int i = 7; i >= 0; i--)
    {
        uint8_t bit = (value >> i) & 0x01;
        sendBit(bit);
    }

    // 3. STOP BIT (LOW) ensures the line goes back to LOW, 
    // guaranteeing a rising edge for the next byte's Start Bit!
    sendBit(LOW); 
}


// ============================================================
// LENGTH ENCODING
//
// Lengths are encoded as little-endian septets.
//
// Example:
//
//     100
//
// becomes:
//
//     01100100
//
// Example:
//
//     128
//
// becomes:
//
//     10000001
//     00000000
//
// MSB = 1 means another septet follows.
// MSB = 0 means this is the final septet.
// ============================================================

bool appendLength(
    uint8_t *buffer,
    int &index,
    uint16_t value
)
{
    do
    {
        if (index >= MAX_DATA_BYTES)
            return false;

        uint8_t septet =
            value & 0x7F;

        value >>= 7;

        if (value != 0)
        {
            septet |= 0x80;
        }

        buffer[index++] = septet;

    } while (value != 0);

    return true;
}


// ============================================================
// BUILD APPLICATION-LAYER DATA
//
// Result:
//
//     [MIME length]
//     [MIME]
//     [payload length]
//     [payload]
// ============================================================

int buildApplicationPacket(
    const char *payload,
    uint8_t *data
)
{
    int index = 0;


    // --------------------------------------------------------
    // MIME length
    // --------------------------------------------------------

    uint16_t mimeLength =
        strlen(MIME_TYPE);

    if (!appendLength(
            data,
            index,
            mimeLength))
    {
        return -1;
    }


    // --------------------------------------------------------
    // MIME string
    // --------------------------------------------------------

    for (int i = 0; i < mimeLength; i++)
    {
        if (index >= MAX_DATA_BYTES)
            return -1;

        data[index++] =
            MIME_TYPE[i];
    }


    // --------------------------------------------------------
    // Payload length
    // --------------------------------------------------------

    uint16_t payloadLength =
        strlen(payload);

    if (!appendLength(
            data,
            index,
            payloadLength))
    {
        return -1;
    }


    // --------------------------------------------------------
    // Payload
    // --------------------------------------------------------

    for (int i = 0; i < payloadLength; i++)
    {
        if (index >= MAX_DATA_BYTES)
            return -1;

        data[index++] =
            payload[i];
    }


    return index;
}


void sendPacket(const char *payload)
{
    uint8_t data[MAX_DATA_BYTES];

    uint8_t codeword[
        MAX_DATA_BYTES + RS_ECC_SYMBOLS
    ];


    // --------------------------------------------------------
    // Build application layer
    // --------------------------------------------------------

    int dataLength =
        buildApplicationPacket(
            payload,
            data
        );


    if (dataLength < 0)
    {
        Serial.println(
            "ERROR: packet too large."
        );

        return;
    }


    // --------------------------------------------------------
    // Reed-Solomon encoding
    // --------------------------------------------------------

    rsEncode(
        data,
        dataLength,
        codeword
    );


    int codewordLength =
        dataLength + RS_ECC_SYMBOLS;


    // --------------------------------------------------------
    // START
    // --------------------------------------------------------

    Serial.println("Sending packet...");

    nextTxTime = micros(); 

    for (int i = 0; i < START_BITS; i++)
    {
        sendBit(START_BIT);
    }

    // before the first byte's Start Bit!
    sendBit(LOW); 

    // --------------------------------------------------------
    // DATA + ECC
    // --------------------------------------------------------

    for (int i = 0; i < codewordLength; i++)
    {
        sendByte(codeword[i]);
    }


    // --------------------------------------------------------
    // STOP
    // --------------------------------------------------------

    for (int i = 0; i < STOP_BITS; i++)
    {
        sendBit(STOP_BIT);
    }


    // Return line to idle LOW.
    digitalWrite(DATA_PIN, LOW);


    // Debug output.
    Serial.print("Payload: ");
    Serial.println(payload);

    Serial.print("Data bytes: ");
    Serial.println(dataLength);

    Serial.print("ECC bytes: ");
    Serial.println(RS_ECC_SYMBOLS);

    Serial.print("Total codeword bytes: ");
    Serial.println(codewordLength);
}


// ============================================================
// SETUP
// ============================================================

void setup()
{
    pinMode(
        DATA_PIN,
        OUTPUT
    );

    // Idle state.
    digitalWrite(
        DATA_PIN,
        LOW
    );


    Serial.begin(115200);


    initGaloisField();


    Serial.println();
    Serial.println(
        "Custom protocol transmitter ready."
    );

    Serial.print(
        "BIT_PERIOD_US = "
    );

    Serial.println(
        BIT_PERIOD_US
    );
}


// ============================================================
// MAIN LOOP
// ============================================================

void loop()
{
    const char payload[] = "Hello World";


    sendPacket(payload);

    delay(3000); // just a delay for debugging.
}