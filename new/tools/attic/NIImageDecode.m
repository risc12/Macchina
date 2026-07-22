//
//  NIImageDecode.m
//  Macchina — NITooling
//

#import "NIImageDecode.h"

// Expand a 5-bit level (0..31) to 8-bit (0..255), rounding.
static inline uint8_t Expand5To8(uint8_t v) { return (uint8_t)((v * 255 + 15) / 31); }

NSData * NIST7529DataToGray8(uint16_t width, uint16_t height, NSData * encoded)
{
    size_t pixels = (size_t)width * height;
    size_t groups = pixels / 3;             // encoder packs 3 pixels per 2 bytes

    if (encoded.length < groups * 2)
        return nil;

    const uint8_t * src  = encoded.bytes;
    uint8_t       * gray = (uint8_t *)malloc(pixels ? pixels : 1);

    for (size_t g = 0; g < groups; g++)
    {
        uint8_t b0 = src[g * 2];
        uint8_t b1 = src[g * 2 + 1];

        // Inverse of the bit layout in NI24BPPToST7529Data:
        //   byte0 = [ p0:5 ][ p1_hi:3 ]
        //   byte1 = [ p1_lo:2 ][ unused:1 ][ p2:5 ]
        uint8_t p0 =  b0 >> 3;
        uint8_t p1 = ((b0 & 0x07) << 2) | (b1 >> 6);
        uint8_t p2 =  b1 & 0x1f;

        gray[g * 3 + 0] = Expand5To8(p0);
        gray[g * 3 + 1] = Expand5To8(p1);
        gray[g * 3 + 2] = Expand5To8(p2);
    }

    // Any trailing pixels the packing couldn't cover (pixels % 3) stay 0.
    for (size_t i = groups * 3; i < pixels; i++)
        gray[i] = 0;

    return [NSData dataWithBytesNoCopy:gray length:pixels freeWhenDone:YES];
}

BOOL NIST7529WritePGM(uint16_t width, uint16_t height, NSData * encoded, NSString * path)
{
    NSData * gray = NIST7529DataToGray8(width, height, encoded);
    if (gray == nil)
        return NO;

    NSMutableData * pgm = [NSMutableData data];
    NSString * header = [NSString stringWithFormat:@"P5\n%u %u\n255\n", width, height];
    [pgm appendData:[header dataUsingEncoding:NSASCIIStringEncoding]];
    [pgm appendData:gray];

    return [pgm writeToFile:path atomically:YES];
}
