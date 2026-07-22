//
//  NIDisplayDrawCapture.m
//  Macchina — NITooling
//

#import "NIDisplayDrawCapture.h"
#import "NIHexDump.h"
#import "NIImageDecode.h"

// Offsets mirror NIDisplayDrawMessage -dataRepresentation exactly:
//   0x00 msgid   0x04 dispn   0x08 y   0x0a x   0x0c h   0x0e w   0x10 size
//   0x14 payload...
// sizeof(struct packet) is 20 (0x14); the 0x10000000 bit OR'd into dispn on the
// way out is a marker we strip back off here.
enum { kDrawHeaderLen = 0x14, kDrawDisplayMarker = 0x10000000u };

@implementation NIDisplayDrawMessage (Inbound)

+ (NIMessage *)messageFromData:(NSData *)data
{
    if (data.length < kDrawHeaderLen)
        return nil;

    const uint8_t * bytes = data.bytes;

    NIDisplayDrawMessage * m = [NIDisplayDrawMessage new];
    m.displayNumber = *(uint32_t *)(bytes + 0x04) & ~kDrawDisplayMarker;
    m.originY       = *(uint16_t *)(bytes + 0x08);
    m.originX       = *(uint16_t *)(bytes + 0x0a);
    m.sizeHeight    = *(uint16_t *)(bytes + 0x0c);
    m.sizeWidth     = *(uint16_t *)(bytes + 0x0e);

    uint32_t   size      = *(uint32_t *)(bytes + 0x10);
    NSUInteger available = data.length - kDrawHeaderLen;
    NSUInteger payloadLen = (size < available) ? size : available;  // clamp to what arrived

    m.st7529EncodedImage = [data subdataWithRange:NSMakeRange(kDrawHeaderLen, payloadLen)];

    return m;
}

@end

@implementation NIControllerRequestServer (Capture)

- (NSData *)handleNIDisplayDrawMessage:(NIDisplayDrawMessage *)message
{
    NSData * payload = message.st7529EncodedImage;

    NSLog(@"    display draw: display %u  rect {x:%u y:%u w:%u h:%u}  payload %lu bytes",
          message.displayNumber,
          message.originX, message.originY,
          message.sizeWidth, message.sizeHeight,
          (unsigned long)payload.length);
    NSLog(@"%@", NIHexDump(payload));

    // If a capture dir is set, decode the payload to a viewable PGM so we can
    // eyeball the actual bit depth / packing NI uses.
    const char * dir = getenv("MACCHINA_CAPTURE_DIR");
    if (dir != NULL)
    {
        static unsigned long seq = 0;
        NSString * path = [NSString stringWithFormat:@"%s/draw-%03lu-d%u-%ux%u.pgm",
                           dir, seq++, message.displayNumber,
                           message.sizeWidth, message.sizeHeight];

        if (NIST7529WritePGM(message.sizeWidth, message.sizeHeight, payload, path))
            NSLog(@"    wrote %@", path);
        else
            NSLog(@"    (could not decode payload to PGM — likely not the assumed ST7529 packing)");
    }

    // Return a plausible ACK. NIControllerRequestServer's other handlers all
    // answer 'true'; without a reply NI's software may stall waiting for one.
    uint32_t theTrue = 'true';
    return [NSData dataWithBytes:&theTrue length:sizeof(theTrue)];
}

@end
