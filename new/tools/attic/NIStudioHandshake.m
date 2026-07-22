//
//  NIStudioHandshake.m
//  Macchina — NITooling
//

#import "NIStudioHandshake.h"
#import "NIHexDump.h"

@implementation NIMainHandlerServer (StudioHandshake)

- (NSData *)handleNIUnknownMessage:(NIMessage *)message
{
    NSData   * raw = [message dataRepresentation];
    // Match on the 3-byte tag (id & 0xffffff); the top byte is the negotiated
    // protocol version (0x02 → 0x03 once we answer GetServiceVersion), so the
    // same message arrives under either prefix.
    uint32_t   tag = message.messageId & 0x00ffffff;

    // 0xNN536756 — GetServiceVersion. Real agent replies with 8 bytes:
    // version 0x00020802 + count 3 (captured from live oracle).
    if (tag == 0x536756)
    {
        uint32_t reply[2] = { 0x00020802, 0x00000003 };
        return [NSData dataWithBytes:reply length:sizeof(reply)];
    }

    // 0xNN447500 "Du" — the v3 CONNECT (not a mere presence poll): u32 usb
    // product id at 0x04, client tag at 0x08 ('NiM2'), role at 0x0c ('prmy').
    // The real agent replies with 'true' + a request/notification port-name
    // pair (same layout as the v2 0x02444300 DeviceConnect). We hand back the
    // port our capture-enabled NIControllerRequestServer already listens on, so
    // Maschine 2's subsequent requests (incl. display draws) land in our tap.
    if (tag == 0x447500 && raw.length >= 0x10)
    {
        uint32_t productId = *(uint32_t *)(raw.bytes + 0x04);
        uint32_t clientTag = *(uint32_t *)(raw.bytes + 0x08);
        uint32_t role      = *(uint32_t *)(raw.bytes + 0x0c);

        BOOL present = (productId == kNIStudioProductId);
        NSLog(@"    v3 connect 0x%04x from %c%c%c%c/%c%c%c%c -> %@",
              productId,
              (clientTag >> 24) & 0xff, (clientTag >> 16) & 0xff,
              (clientTag >>  8) & 0xff,  clientTag        & 0xff,
              (role >> 24) & 0xff, (role >> 16) & 0xff,
              (role >>  8) & 0xff,  role        & 0xff,
              present ? @"present" : @"absent");

        if (!present)
        {
            uint32_t reply = 0;
            return [NSData dataWithBytes:&reply length:sizeof(reply)];
        }

        // Build the 'true' + port-pair reply (mirrors the captured 63-byte form).
        const char *inName  = "NIHWMaschineController0001Request";
        const char *outName = "NIHWMaschineController0001Notification";
        uint32_t inLen  = (uint32_t)strlen(inName);
        uint32_t outLen = (uint32_t)strlen(outName);
        uint32_t total  = 0x0c + inLen + 1 + outLen + 1;

        uint8_t reply[total];
        memset(reply, 0, total);
        *((uint32_t *)(reply + 0x00)) = 'true';
        *((uint32_t *)(reply + 0x04)) = inLen + 1;
        memcpy(reply + 0x08, inName, inLen);
        *((uint32_t *)(reply + 0x08 + inLen + 1)) = outLen + 1;
        memcpy(reply + 0x0c + inLen + 1, outName, outLen);
        return [NSData dataWithBytes:reply length:total];
    }

    NSLog(@" --- unhandled unknown message on %@:\n%@", self.name, NIDescribeFrame(raw));
    return nil;
}

@end
