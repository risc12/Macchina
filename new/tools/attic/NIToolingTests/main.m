//
//  main.m
//  NIToolingTests
//
//  Plain Foundation test runner (no XCTest) so it builds and runs headless via
//  build.sh, matching the project's IDE-independent feedback loop. Covers the
//  pure logic that's testable without hardware: the ST7529 encode/decode
//  round-trip, the NIDisplayDrawMessage serialize/parse round-trip, and capture
//  file parsing.
//

#import <Foundation/Foundation.h>
#import "NIImageConversions.h"       // NI24BPPToST7529Data (encoder under test)
#import "NIImageDecode.h"            // NIST7529DataToGray8 (decoder under test)
#import "NIProtocolMessages.h"
#import "NIDisplayDrawCapture.h"     // +messageFromData: category
#import "NIHexDump.h"
#import "NIReplay.h"

static int gChecks = 0;
static int gFailures = 0;

#define CHECK(cond, desc) do {                                   \
    gChecks++;                                                   \
    if (cond) { fprintf(stderr, "  ok   %s\n", desc); }          \
    else      { gFailures++; fprintf(stderr, "  FAIL %s\n", desc); } \
} while (0)

static void TestEncodeDecodeRoundTrip(void)
{
    fprintf(stderr, "ST7529 encode/decode round-trip:\n");

    // 6 px (divisible by 3): white, black, white, black, white, black.
    uint8_t bitmap[6 * 3] = {
        255,255,255,   0,0,0,   255,255,255,
        0,0,0,   255,255,255,   0,0,0,
    };
    NSData * encoded = NI24BPPToST7529Data(6, 1, bitmap);
    CHECK(encoded.length == 6 / 3 * 2, "encoded length is pixels/3*2");

    NSData * gray = NIST7529DataToGray8(6, 1, encoded);
    CHECK(gray.length == 6, "decoded gray length is width*height");

    const uint8_t * g = gray.bytes;
    uint8_t expected[6] = { 255, 0, 255, 0, 255, 0 };
    BOOL match = YES;
    for (int i = 0; i < 6; i++)
        if (g[i] != expected[i]) match = NO;
    CHECK(match, "black/white pixels survive the round-trip");
}

static void TestDecoderGrayLevels(void)
{
    fprintf(stderr, "ST7529 decoder intermediate levels:\n");

    // One packed group with 5-bit levels p0=0, p1=16, p2=31.
    //   byte0 = (p0<<3) | (p1>>2)          = 0x04
    //   byte1 = ((p1&3)<<6) | p2           = 0x1f
    uint8_t packed[2] = { 0x04, 0x1f };
    NSData * gray = NIST7529DataToGray8(3, 1, [NSData dataWithBytes:packed length:2]);
    const uint8_t * g = gray.bytes;

    CHECK(g[0] == 0,   "level 0 -> 0");
    CHECK(g[1] == 132, "level 16 -> 132 (mid gray survives)");
    CHECK(g[2] == 255, "level 31 -> 255");
}

static void TestDecoderRejectsShort(void)
{
    fprintf(stderr, "ST7529 decoder guards short input:\n");
    NSData * tiny = [NSData dataWithBytes:(uint8_t[]){0x00} length:1];
    CHECK(NIST7529DataToGray8(6, 1, tiny) == nil, "too-short payload returns nil");
}

static void TestDisplayDrawRoundTrip(void)
{
    fprintf(stderr, "NIDisplayDrawMessage serialize/parse round-trip:\n");

    uint8_t payloadBytes[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    NSData * payload = [NSData dataWithBytes:payloadBytes length:8];

    NIDisplayDrawMessage * out = [NIDisplayDrawMessage new];
    out.displayNumber      = 1;
    out.originX            = 10;
    out.originY            = 20;
    out.sizeWidth          = 30;
    out.sizeHeight         = 40;
    out.st7529EncodedImage = payload;

    NSData * wire = [out dataRepresentation];
    NIDisplayDrawMessage * in = (NIDisplayDrawMessage *)[NIDisplayDrawMessage messageFromData:wire];

    CHECK(in != nil,                       "parsed a message back");
    CHECK(in.displayNumber == 1,           "displayNumber (marker bit stripped)");
    CHECK(in.originX == 10,                "originX");
    CHECK(in.originY == 20,                "originY");
    CHECK(in.sizeWidth == 30,             "sizeWidth");
    CHECK(in.sizeHeight == 40,            "sizeHeight");
    CHECK([in.st7529EncodedImage isEqualToData:payload], "payload bytes preserved");
}

static void TestCaptureFileParsing(void)
{
    fprintf(stderr, "Capture file parsing:\n");

    // Two length-prefixed records.
    NSMutableData * file = [NSMutableData data];
    uint8_t a[3] = { 0xaa, 0xbb, 0xcc };
    uint8_t b[2] = { 0x11, 0x22 };
    uint32_t la = 3, lb = 2;
    [file appendBytes:&la length:4]; [file appendBytes:a length:3];
    [file appendBytes:&lb length:4]; [file appendBytes:b length:2];

    NSString * path = [NSTemporaryDirectory() stringByAppendingPathComponent:@"nitool-capture.bin"];
    [file writeToFile:path atomically:YES];

    NSArray<NSData *> * frames = NIReadCaptureFile(path);
    CHECK(frames.count == 2, "two frames parsed");
    CHECK(frames.count == 2 && ((NSData *)frames[0]).length == 3, "first frame length");
    CHECK(frames.count == 2 && ((NSData *)frames[1]).length == 2, "second frame length");

    [[NSFileManager defaultManager] removeItemAtPath:path error:NULL];
}

static void TestHexDumpSmoke(void)
{
    fprintf(stderr, "Hexdump / describe smoke:\n");
    uint8_t frame[8] = { 0x44, 0x73, 0x64, 0x02, 0, 0, 0, 0 }; // 0x02647344 LE
    NSString * desc = NIDescribeFrame([NSData dataWithBytes:frame length:8]);
    CHECK([desc rangeOfString:@"NIDisplayDrawMessage"].location != NSNotFound,
          "describe maps id 0x02647344 -> NIDisplayDrawMessage");
}

int main(int argc, const char * argv[])
{
    @autoreleasepool
    {
        TestEncodeDecodeRoundTrip();
        TestDecoderGrayLevels();
        TestDecoderRejectsShort();
        TestDisplayDrawRoundTrip();
        TestCaptureFileParsing();
        TestHexDumpSmoke();

        fprintf(stderr, "\n%d checks, %d failure(s)\n", gChecks, gFailures);
    }
    return gFailures == 0 ? 0 : 1;
}
