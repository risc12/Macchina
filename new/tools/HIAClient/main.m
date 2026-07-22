//
//  main.m
//  MacchinaHIAClient — NewClients/HostIntegrationAgentClient
//
//  Connects to NIHostIntegrationAgent — the daemon that actually renders and
//  drives the Studio's screens (docs/studio-investigation.md §5). Apps reach
//  it on the service port `com.native-instruments.NIHostIntegrationAgent`,
//  and its own IPCConnection::establish shows the intended client flow (§6):
//  send a MsgDeviceConnect (with a UTF-16 id string), read back an in/out
//  port pair, then answer the MsgConnectionEstablished completion.
//
//  The app→HIA connect frame is not fully decoded, so this client is an
//  instrument: it probes the known-candidate layouts in order and hexdumps
//  every reply. First one that answers 'true' + a port pair wins and the
//  establish instrumentation (MACCHINA_ESTABLISH_REPLY) takes over.
//
//    A. DeviceConnect with UTF-16LE id string (what setIDString suggests)
//    B. DeviceConnect with ASCII serial (the HardwareAgent layout)
//    C. plain Du subscribe (same embedded IPCServer code, may just work)
//
//  Usage: MacchinaHIAClient [productIdHex [serial]]
//  Defaults: product 0x1300 (Maschine Studio), serial 39195855.
//  Needs NIHostIntegrationAgent running (NI's installer keeps it resident;
//  check with: launchctl list | grep -i native).
//

#import <Foundation/Foundation.h>
#import "NIAgentConnection.h"

// Bulk display draw (Studio protocol, decoded in LEARNINGS.md E12): a 0x647344
// envelope whose payload is WriteWindowRequest + pixels + EndOfUpdateRequest.
static void AppendBE16(NSMutableData * d, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    [d appendBytes:b length:2];
}

static NSData * BulkDrawFrame(uint32_t messageId, uint32_t display,
                             uint16_t width, uint16_t height,
                             uint8_t dataFormat, uint16_t pixel)
{
    uint32_t pixelCount  = (uint32_t)width * height;
    uint32_t pixelBytes  = pixelCount * 2;
    uint32_t payloadSize = 16 + pixelBytes + 4;

    NSMutableData * frame = [NSMutableData data];
    uint32_t header[5] = { messageId, display, 0,
                           ((uint32_t)width << 16) | height, payloadSize };
    [frame appendBytes:header length:sizeof(header)];

    uint8_t ww[8] = { 0x84, 0x00, (uint8_t)display, dataFormat, 0, 0, 0, 0 };
    [frame appendBytes:ww length:sizeof(ww)];
    AppendBE16(frame, 0); AppendBE16(frame, 0);
    AppendBE16(frame, width); AppendBE16(frame, height);

    NSMutableData * payload = [NSMutableData dataWithLength:pixelBytes];
    uint16_t * px = payload.mutableBytes;
    for (uint32_t i = 0; i < pixelCount; i++) px[i] = pixel;
    [frame appendData:payload];

    uint8_t eou[4] = { 0x40, 0x00, (uint8_t)display, 0x00 };
    [frame appendBytes:eou length:sizeof(eou)];
    return frame;
}

static void BulkDrawBothDisplays(NIAgentConnection * conn, NIAgentEndpoints * ep)
{
    uint16_t colors[2] = { 0xF800, 0x001F };   // red, blue
    for (uint32_t display = 0; display < 2; display++)
    {
        NSData * frame = BulkDrawFrame([conn messageIdForTag:kNITagDisplayDraw],
                                       display, 480, 272, 0x60, colors[display]);
        NSData * reply = [ep.request sendFrame:frame];
        uint32_t v = 0;
        if (reply.length >= 4) memcpy(&v, reply.bytes, sizeof(v));
        NSLog(@"    bulk draw display %u on %@ -> reply 0x%08x (%lu bytes)",
              display, ep.label, v, (unsigned long)reply.length);
    }
}

int main(int argc, const char * argv[])
{
    @autoreleasepool
    {
        uint32_t   productId = 0x1300;
        NSString * serial    = @"39195855";

        if (argc > 1)
            productId = (uint32_t)strtoul(argv[1], NULL, 16);
        if (argc > 2)
            serial = @(argv[2]);

        NIAgentConnection * conn =
            [NIAgentConnection connectionToService:@"com.native-instruments.NIHostIntegrationAgent"];
        if (conn == nil)
        {
            fprintf(stderr, "com.native-instruments.NIHostIntegrationAgent not found — "
                            "NIHostIntegrationAgent is not running.\n");
            return 1;
        }

        NSLog(@"=== version negotiation (probe — HIA may or may not answer) ===");
        [conn negotiateVersion];   // keep probing even if this fails

        NIAgentEndpoints * endpoints = nil;

        NSLog(@"=== candidate A: DeviceConnect, UTF-16LE id string ===");
        NIDeviceConnectResponse * reply = [conn connectToProduct:productId
                                                       clientTag:kNIClientTagMaschine2
                                                            role:kNIClientRolePrimary
                                                   utf16IdString:serial];
        endpoints = [conn openEndpointsFor:reply label:@"hia-utf16"];

        if (endpoints == nil)
        {
            NSLog(@"=== candidate B: DeviceConnect, ASCII serial (HardwareAgent layout) ===");
            reply     = [conn connectToProduct:productId
                                     clientTag:kNIClientTagMaschine2
                                          role:kNIClientRolePrimary
                                        serial:serial];
            endpoints = [conn openEndpointsFor:reply label:@"hia-ascii"];
        }

        if (endpoints == nil)
        {
            NSLog(@"=== candidate C: Du subscribe ===");
            reply     = [conn subscribeToProduct:productId
                                       clientTag:kNIClientTagMaschine2
                                            role:kNIClientRolePrimary];
            endpoints = [conn openEndpointsFor:reply label:@"hia-subscribe"];
            if (endpoints)
            {
                [conn registerNotificationPort:endpoints];
                [conn sendSubscribeVoidOp:endpoints];
            }
        }

        if (endpoints == nil)
        {
            fprintf(stderr, "no candidate produced a port pair — the replies hexdumped above "
                            "are the decode material. Next step per docs/studio-investigation.md "
                            "§8: capture a real app connect against a debuggable HIA copy.\n");
            return 1;
        }

        NSLog(@"=== connected via '%@'; drawing after 1.5 s, then staying alive ===",
              endpoints.label);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            NSLog(@"--- bulk draw on the subscription port ---");
            BulkDrawBothDisplays(conn, endpoints);
        });
        [[NSRunLoop currentRunLoop] run];
    }
    return 0;
}
