/*
 * QR Code generator library (C)
 *
 * Copyright (c) Project Nayuki. (MIT License)
 * https://www.nayuki.io/page/qr-code-generator-library
 */

#include "qrcodegen.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Forward declarations */
static bool getBit(uint8_t x, int i);
static void setBit(uint8_t *x, int i, bool b);
static int getNumDataCodewords(int ver, enum qrcodegen_Ecc ecl);
static int getNumRawDataModules(int ver);

static const int8_t ECC_CODEWORDS_PER_BLOCK[4][41] = {
    // Version: 0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40
    {-1,  7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28, 28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},  // Low
    {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26, 26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28},  // Medium
    {-1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30, 28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},  // Quartile
    {-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28, 30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},  // High
};

static const int8_t NUM_ERROR_CORRECTION_BLOCKS[4][41] = {
    // Version: 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40
    {-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4,  4,  4,  4,  4,  6,  6,  6,  6,  7,  8,  8,  9,  9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25},  // Low
    {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5,  5,  8,  9,  9, 10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49},  // Medium
    {-1, 1, 1, 2, 2, 4, 4, 6, 6, 8, 8,  8, 10, 12, 16, 12, 17, 16, 18, 21, 20, 23, 23, 25, 27, 29, 34, 34, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68},  // Quartile
    {-1, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8, 11, 11, 16, 16, 18, 16, 19, 21, 25, 25, 25, 34, 30, 32, 35, 37, 40, 42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 72, 74, 77},  // High
};

int qrcodegen_getSize(const uint8_t qrcode[]) {
    assert(qrcode != NULL);
    int version = qrcode[0];
    assert(qrcodegen_VERSION_MIN <= version && version <= qrcodegen_VERSION_MAX);
    return version * 4 + 17;
}

bool qrcodegen_getModule(const uint8_t qrcode[], int x, int y) {
    int qrSize = qrcodegen_getSize(qrcode);
    if (0 <= x && x < qrSize && 0 <= y && y < qrSize) {
        int index = y * qrSize + x;
        return getBit(qrcode[(index >> 3) + 1], index & 7);
    } else {
        return false;
    }
}

static bool getBit(uint8_t x, int i) {
    return ((x >> i) & 1) != 0;
}

static void setBit(uint8_t *x, int i, bool b) {
    if (b)
        *x |= 1u << i;
    else
        *x &= ~(1u << i);
}

static void setModule(uint8_t qrcode[], int x, int y, bool isDark) {
    int qrSize = qrcodegen_getSize(qrcode);
    assert(0 <= x && x < qrSize && 0 <= y && y < qrSize);
    int index = y * qrSize + x;
    setBit(&qrcode[(index >> 3) + 1], index & 7, isDark);
}

static void setModuleBounded(uint8_t qrcode[], int x, int y, bool isDark) {
    int qrSize = qrcodegen_getSize(qrcode);
    if (0 <= x && x < qrSize && 0 <= y && y < qrSize)
        setModule(qrcode, x, y, isDark);
}

static bool getModuleBounded(const uint8_t qrcode[], int x, int y) {
    int qrSize = qrcodegen_getSize(qrcode);
    return (0 <= x && x < qrSize && 0 <= y && y < qrSize) && qrcodegen_getModule(qrcode, x, y);
}

static int getNumRawDataModules(int ver) {
    assert(qrcodegen_VERSION_MIN <= ver && ver <= qrcodegen_VERSION_MAX);
    int size = ver * 4 + 17;
    int result = size * size;
    result -= 8 * 8 * 3;  // Finder patterns and separators
    result -= (size - 16) * 2;  // Timing patterns
    result -= 1;  // Dark module
    result -= 15 * 2;  // Format information
    result -= 3 * 3;  // Alignment pattern over timing pattern
    if (ver >= 2) {
        int numAlign = ver / 7 + 2;
        result -= (numAlign * numAlign - 3) * 25;  // Alignment patterns
        result += (numAlign - 2) * 2 * 5;  // Alignment patterns over timing patterns
        if (ver >= 7)
            result -= 6 * 3 * 2;  // Version information
    }
    return result;
}

static int getNumDataCodewords(int ver, enum qrcodegen_Ecc ecl) {
    return getNumRawDataModules(ver) / 8 - ECC_CODEWORDS_PER_BLOCK[(int)ecl][ver] * NUM_ERROR_CORRECTION_BLOCKS[(int)ecl][ver];
}

static void reedSolomonComputeDivisor(int degree, uint8_t result[]) {
    assert(1 <= degree && degree <= 67);
    memset(result, 0, (size_t)degree * sizeof(result[0]));
    result[degree - 1] = 1;

    uint8_t root = 1;
    for (int i = 0; i < degree; i++) {
        for (int j = 0; j < degree; j++) {
            uint8_t prod = 0;
            if (result[j] != 0 && root != 0) {
                // GF(256) multiply
                uint8_t a = result[j];
                uint8_t b = root;
                uint8_t p = 0;
                for (int bit = 0; bit < 8; bit++) {
                    if ((b & 1) != 0) p ^= a;
                    uint8_t hi = (uint8_t)(a & 0x80);
                    a = (uint8_t)(a << 1);
                    if (hi != 0) a ^= 0x1D;
                    b >>= 1;
                }
                prod = p;
            }
            result[j] = prod;
            if (j + 1 < degree)
                result[j] ^= result[j + 1];
        }
        // Multiply root by 2 in GF(256)
        uint8_t hi = (uint8_t)(root & 0x80);
        root = (uint8_t)(root << 1);
        if (hi != 0) root ^= 0x1D;
    }
}

static void reedSolomonComputeRemainder(const uint8_t data[], int dataLen,
        const uint8_t generator[], int degree, uint8_t result[]) {
    assert(1 <= degree && degree <= 67);
    memset(result, 0, (size_t)degree * sizeof(result[0]));
    for (int i = 0; i < dataLen; i++) {
        uint8_t factor = data[i] ^ result[0];
        memmove(&result[0], &result[1], (size_t)(degree - 1) * sizeof(result[0]));
        result[degree - 1] = 0;
        for (int j = 0; j < degree; j++) {
            if (factor != 0 && generator[j] != 0) {
                uint8_t a = generator[j];
                uint8_t b = factor;
                uint8_t p = 0;
                for (int bit = 0; bit < 8; bit++) {
                    if ((b & 1) != 0) p ^= a;
                    uint8_t hi = (uint8_t)(a & 0x80);
                    a = (uint8_t)(a << 1);
                    if (hi != 0) a ^= 0x1D;
                    b >>= 1;
                }
                result[j] ^= p;
            }
        }
    }
}

static void appendBitsToBuffer(unsigned int val, int numBits, uint8_t buffer[], int *bitLen) {
    for (int i = numBits - 1; i >= 0; i--) {
        buffer[*bitLen >> 3] |= (uint8_t)(((val >> i) & 1) << (7 - (*bitLen & 7)));
        (*bitLen)++;
    }
}

static void drawFinderPattern(uint8_t qrcode[], int x, int y) {
    for (int dy = -4; dy <= 4; dy++) {
        for (int dx = -4; dx <= 4; dx++) {
            int dist = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
            int xx = x + dx, yy = y + dy;
            if (dist == 2 || dist == 4)
                setModuleBounded(qrcode, xx, yy, false);
            else if (dist <= 1 || dist == 3)
                setModuleBounded(qrcode, xx, yy, true);
        }
    }
}

static void drawAlignmentPattern(uint8_t qrcode[], int x, int y) {
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            int dist = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
            setModuleBounded(qrcode, x + dx, y + dy, dist != 1);
        }
    }
}

static int getAlignmentPatternPositions(int ver, uint8_t result[7]) {
    if (ver == 1)
        return 0;
    int num = ver / 7 + 2;
    int step = (ver == 32) ? 26 : (ver * 4 + num * 2 + 1) / (num * 2 - 2) * 2;
    for (int i = num - 1, pos = ver * 4 + 10; i >= 1; i--, pos -= step)
        result[i] = (uint8_t)pos;
    result[0] = 6;
    return num;
}

static void drawFunctionPatterns(uint8_t qrcode[], int ver) {
    int qrSize = ver * 4 + 17;
    // Timing patterns
    for (int i = 0; i < qrSize; i++) {
        setModule(qrcode, 6, i, i % 2 == 0);
        setModule(qrcode, i, 6, i % 2 == 0);
    }
    // Finder patterns
    drawFinderPattern(qrcode, 3, 3);
    drawFinderPattern(qrcode, qrSize - 4, 3);
    drawFinderPattern(qrcode, 3, qrSize - 4);

    // Alignment patterns
    uint8_t alignPos[7];
    int numAlign = getAlignmentPatternPositions(ver, alignPos);
    for (int i = 0; i < numAlign; i++) {
        for (int j = 0; j < numAlign; j++) {
            if ((i == 0 && j == 0) || (i == 0 && j == numAlign - 1) || (i == numAlign - 1 && j == 0))
                continue;
            drawAlignmentPattern(qrcode, alignPos[i], alignPos[j]);
        }
    }
    // Dark module
    setModule(qrcode, 8, ver * 4 + 9, true);
}

static void drawFormatBits(uint8_t qrcode[], enum qrcodegen_Ecc ecl, enum qrcodegen_Mask mask) {
    int data = ((int)ecl ^ 1) << 3 | (int)mask;
    int rem = data;
    for (int i = 0; i < 10; i++)
        rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int bits = (data << 10 | rem) ^ 0x5412;

    int qrSize = qrcodegen_getSize(qrcode);
    for (int i = 0; i <= 5; i++)
        setModule(qrcode, 8, i, getBit((uint8_t)(bits >> i), 0));
    setModule(qrcode, 8, 7, getBit((uint8_t)(bits >> 6), 0));
    setModule(qrcode, 8, 8, getBit((uint8_t)(bits >> 7), 0));
    setModule(qrcode, 7, 8, getBit((uint8_t)(bits >> 8), 0));
    for (int i = 9; i < 15; i++)
        setModule(qrcode, 14 - i, 8, getBit((uint8_t)(bits >> i), 0));

    for (int i = 0; i < 8; i++)
        setModule(qrcode, qrSize - 1 - i, 8, getBit((uint8_t)(bits >> i), 0));
    for (int i = 8; i < 15; i++)
        setModule(qrcode, 8, qrSize - 15 + i, getBit((uint8_t)(bits >> i), 0));
    setModule(qrcode, 8, qrSize - 8, true);
}

static bool getMaskPattern(int mask, int x, int y) {
    switch (mask) {
        case 0:  return (x + y) % 2 == 0;
        case 1:  return y % 2 == 0;
        case 2:  return x % 3 == 0;
        case 3:  return (x + y) % 3 == 0;
        case 4:  return (x / 3 + y / 2) % 2 == 0;
        case 5:  return (x * y) % 2 + (x * y) % 3 == 0;
        case 6:  return ((x * y) % 2 + (x * y) % 3) % 2 == 0;
        case 7:  return ((x + y) % 2 + (x * y) % 3) % 2 == 0;
        default: return false;
    }
}

static void drawCodewords(uint8_t qrcode[], const uint8_t data[], int dataLen) {
    int qrSize = qrcodegen_getSize(qrcode);
    int i = 0;
    for (int right = qrSize - 1; right >= 1; right -= 2) {
        if (right == 6) right--;
        for (int vert = 0; vert < qrSize; vert++) {
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                bool upward = ((right + 1) & 2) == 0;
                int y = upward ? qrSize - 1 - vert : vert;
                // Check if module is function pattern
                bool isFunc = (x < 9 && y < 9) || (x >= qrSize - 8 && y < 9) || (x < 9 && y >= qrSize - 8) ||
                              (x == 6) || (y == 6);
                if (isFunc) continue;
                bool dark = false;
                if (i < dataLen * 8) {
                    dark = getBit(data[i >> 3], 7 - (i & 7));
                    i++;
                }
                setModule(qrcode, x, y, dark);
            }
        }
    }
}

static void applyMask(uint8_t qrcode[], int mask) {
    int qrSize = qrcodegen_getSize(qrcode);
    for (int y = 0; y < qrSize; y++) {
        for (int x = 0; x < qrSize; x++) {
            bool isFunc = (x < 9 && y < 9) || (x >= qrSize - 8 && y < 9) || (x < 9 && y >= qrSize - 8) ||
                          (x == 6) || (y == 6);
            if (isFunc) continue;
            bool invert = getMaskPattern(mask, x, y);
            bool val = qrcodegen_getModule(qrcode, x, y);
            setModule(qrcode, x, y, val ^ invert);
        }
    }
}

bool qrcodegen_encodeText(const char *text, uint8_t tempBuffer[], uint8_t qrcode[],
        enum qrcodegen_Ecc ecl, int minVersion, int maxVersion, enum qrcodegen_Mask mask, bool boostEcl) {
    size_t textLen = strlen(text);
    if (minVersion < 1) minVersion = 1;
    if (maxVersion > 10) maxVersion = 10; // Cap to Version 10 (57x57) to optimize embedded RAM

    // Find smallest version that fits
    int ver = -1;
    int dataCapacityBits = 0;
    for (int v = minVersion; v <= maxVersion; v++) {
        int cap = getNumDataCodewords(v, ecl) * 8;
        int headerAndLenBits = 4 + (v <= 9 ? 8 : 16);
        if ((int)textLen * 8 + headerAndLenBits <= cap) {
            ver = v;
            dataCapacityBits = cap;
            break;
        }
    }
    if (ver == -1) return false;

    // Build data bit stream (Mode 4: Byte mode)
    memset(tempBuffer, 0, (size_t)(dataCapacityBits / 8 + 1));
    int bitLen = 0;
    appendBitsToBuffer(0x4, 4, tempBuffer, &bitLen); // Byte mode indicator
    appendBitsToBuffer((unsigned int)textLen, ver <= 9 ? 8 : 16, tempBuffer, &bitLen);
    for (size_t i = 0; i < textLen; i++)
        appendBitsToBuffer((uint8_t)text[i], 8, tempBuffer, &bitLen);

    // Terminator
    int numTerm = dataCapacityBits - bitLen < 4 ? dataCapacityBits - bitLen : 4;
    appendBitsToBuffer(0, numTerm, tempBuffer, &bitLen);
    // Pad to byte
    appendBitsToBuffer(0, (8 - (bitLen % 8)) % 8, tempBuffer, &bitLen);
    // Pad bytes 0xEC, 0x11
    uint8_t pad = 0xEC;
    while (bitLen < dataCapacityBits) {
        appendBitsToBuffer(pad, 8, tempBuffer, &bitLen);
        pad = pad == 0xEC ? 0x11 : 0xEC;
    }

    // Error correction
    int numBlocks = NUM_ERROR_CORRECTION_BLOCKS[(int)ecl][ver];
    int blockEccLen = ECC_CODEWORDS_PER_BLOCK[(int)ecl][ver];
    int rawCodewords = getNumRawDataModules(ver) / 8;
    int dataCodewords = getNumDataCodewords(ver, ecl);
    int shortBlockData = dataCodewords / numBlocks;

    uint8_t rsDivisor[70];
    reedSolomonComputeDivisor(blockEccLen, rsDivisor);

    // Interleave data and ECC codewords
    uint8_t *finalData = qrcode + 1; // Temporarily use qrcode buffer
    memset(finalData, 0, (size_t)rawCodewords);

    // Populate data interleaved
    for (int i = 0; i < shortBlockData; i++) {
        for (int j = 0; j < numBlocks; j++) {
            int blockDataLen = shortBlockData + (j >= numBlocks - dataCodewords % numBlocks ? 1 : 0);
            if (i < blockDataLen) {
                int srcIdx = (j * shortBlockData + (j < numBlocks - dataCodewords % numBlocks ? 0 : j - (numBlocks - dataCodewords % numBlocks))) + i;
                *finalData++ = tempBuffer[srcIdx];
            }
        }
    }

    // Compute ECC for each block
    uint8_t eccBlocks[50][70];
    int dataOffset = 0;
    for (int j = 0; j < numBlocks; j++) {
        int bLen = shortBlockData + (j >= numBlocks - dataCodewords % numBlocks ? 1 : 0);
        reedSolomonComputeRemainder(&tempBuffer[dataOffset], bLen, rsDivisor, blockEccLen, eccBlocks[j]);
        dataOffset += bLen;
    }

    // Interleave ECC
    for (int i = 0; i < blockEccLen; i++) {
        for (int j = 0; j < numBlocks; j++) {
            *finalData++ = eccBlocks[j][i];
        }
    }

    // Initialize QR Code matrix
    qrcode[0] = (uint8_t)ver;
    int qrSize = ver * 4 + 17;
    int bufferBytes = (qrSize * qrSize + 7) / 8;
    // Copy interleaved data to tempBuffer for drawing
    memcpy(tempBuffer, qrcode + 1, (size_t)rawCodewords);
    memset(qrcode + 1, 0, (size_t)bufferBytes);

    drawFunctionPatterns(qrcode, ver);
    drawCodewords(qrcode, tempBuffer, rawCodewords);

    int finalMask = mask == qrcodegen_Mask_AUTO ? 0 : (int)mask;
    applyMask(qrcode, finalMask);
    drawFormatBits(qrcode, ecl, (enum qrcodegen_Mask)finalMask);

    return true;
}
