//
//  NIAgentConnection.m
//  Macchina — NewClients/Shared
//

#import "NIAgentConnection.h"
#import "NIHexDump.h"

static void AppendU32(NSMutableData * d, uint32_t v)
{
    [d appendBytes:&v length:sizeof(v)];
}


@interface NIAgentEndpoints ()
@property (readwrite) NSString        * label;
@property (readwrite) NIRawPortClient * request;
@property (readwrite) NIRawPortServer * notifications;
// onInboundFrame is declared readwrite in the public header.
@end

@implementation NIAgentEndpoints
@end


@interface NIAgentConnection ()
@property (readwrite) NIRawPortClient * mainPort;
@property (readwrite) uint32_t          versionPrefix;
@end

@implementation NIAgentConnection

+ (instancetype)connectionToService:(NSString *)mainPortName;
{
    NIRawPortClient * mainPort = [NIRawPortClient clientWithName:mainPortName];
    if (mainPort == nil)
        return nil;

    NIAgentConnection * zelf = [self new];
    zelf.mainPort      = mainPort;
    zelf.versionPrefix = 0x02;
    return zelf;
}

- (uint32_t)messageIdForTag:(uint32_t)tag;
{
    if (tag & 0xff000000)   // full 4CC message ("SMap", "APP ", …)
        return tag;
    return (self.versionPrefix << 24) | tag;
}

- (NSData *)sendTag:(uint32_t)tag body:(NSData *)body toPort:(NIRawPortClient *)port;
{
    NSMutableData * frame = [NSMutableData data];
    AppendU32(frame, [self messageIdForTag:tag]);
    if (body)
        [frame appendData:body];
    return [port sendFrame:frame];
}

- (NSData *)sendTag:(uint32_t)tag body:(NSData *)body;
{
    return [self sendTag:tag body:body toPort:self.mainPort];
}

- (BOOL)negotiateVersion;
{
    NSData * reply = [self sendTag:kNITagGetServiceVersion body:nil];

    if (reply.length >= 8)
    {
        uint32_t words[2];
        memcpy(words, reply.bytes, sizeof(words));
        self.versionPrefix = 0x03;
        NSLog(@"    service version 0x%08x, count %u — switching to v3 prefix", words[0], words[1]);
        return YES;
    }
    if (reply.length == 4)
    {
        uint32_t v;
        memcpy(&v, reply.bytes, sizeof(v));
        // The v2-form GetServiceVersion (0x02 prefix) returns just the 4-byte
        // service version; the v3-form (0x03) returns {version, count} (8 bytes).
        // Both daemons want the 0x03 prefix, regardless of the (short) version
        // reply. Confirmed by capturing Maschine 2 talking to NIHostIntegration-
        // Agent (LEARNINGS.md E13): HIA answers version 0x00010f00 yet M2 still
        // sends 0x03447500 (Du subscribe) — the earlier "pre-v3, keep 0x02"
        // guess made our HIA connects go out as 0x02… and get refused 'ectl'.
        // (Live: NIHardwareAgent → 0x00020802, NIHostIntegrationAgent → 0x00010f00,
        // both driven with the 0x03 prefix.)
        self.versionPrefix = 0x03;
        NSLog(@"    4-byte version 0x%08x — using 0x03 prefix (both daemons, per M2 capture)", v);
        return YES;
    }

    NSLog(@"    no usable GetServiceVersion reply (%lu bytes)", (unsigned long)reply.length);
    return NO;
}

- (NIDeviceConnectResponse *)parsePortPairReply:(NSData *)reply;
{
    if (reply.length < 4)
        return nil;
    return [NIDeviceConnectResponse responseWithData:reply];
}

- (NIDeviceConnectResponse *)subscribeToProduct:(uint32_t)productId
                                      clientTag:(uint32_t)clientTag
                                           role:(uint32_t)role;
{
    NSMutableData * body = [NSMutableData data];
    AppendU32(body, productId);
    AppendU32(body, clientTag);
    AppendU32(body, role);
    AppendU32(body, 0);

    return [self parsePortPairReply:[self sendTag:kNITagDeviceSubscribe body:body]];
}

// Shared tail of both DeviceConnect variants: [len][bytes][NUL(s)]. The
// length field counts the terminator too (live capture: strlen=9 for
// "39195855" → 29-byte frame).
- (NIDeviceConnectResponse *)connectToProduct:(uint32_t)productId
                                    clientTag:(uint32_t)clientTag
                                         role:(uint32_t)role
                                idStringBytes:(NSData *)idString
                               terminatorSize:(uint32_t)terminatorSize;
{
    NSMutableData * body = [NSMutableData data];
    AppendU32(body, productId);
    AppendU32(body, clientTag);
    AppendU32(body, role);
    AppendU32(body, (uint32_t)idString.length + terminatorSize);
    [body appendData:idString];
    [body increaseLengthBy:terminatorSize];

    return [self parsePortPairReply:[self sendTag:kNITagDeviceConnect body:body]];
}

- (NIDeviceConnectResponse *)connectToProduct:(uint32_t)productId
                                    clientTag:(uint32_t)clientTag
                                         role:(uint32_t)role
                                       serial:(NSString *)serial;
{
    NSData * bytes = [serial dataUsingEncoding:NSASCIIStringEncoding allowLossyConversion:YES];
    return [self connectToProduct:productId clientTag:clientTag role:role
                    idStringBytes:bytes terminatorSize:1];
}

- (NIDeviceConnectResponse *)connectToProduct:(uint32_t)productId
                                    clientTag:(uint32_t)clientTag
                                         role:(uint32_t)role
                                utf16IdString:(NSString *)idString;
{
    NSData * bytes = [idString dataUsingEncoding:NSUTF16LittleEndianStringEncoding];
    return [self connectToProduct:productId clientTag:clientTag role:role
                    idStringBytes:bytes terminatorSize:2];
}

- (NIAgentEndpoints *)openEndpointsFor:(NIDeviceConnectResponse *)response
                                 label:(NSString *)label;
{
    if (response == nil || response.success == NO)
    {
        NSLog(@"[%@] connect refused — no endpoints to open", label);
        return nil;
    }

    NIRawPortClient * request = [NIRawPortClient clientWithName:response.inPortName];
    if (request == nil)
    {
        NSLog(@"[%@] agent assigned request port '%@' but it doesn't exist", label, response.inPortName);
        return nil;
    }

    // Create the endpoints object up front so the receive-port handler can route
    // post-handshake frames to its onInboundFrame block. Captured weakly to avoid
    // an endpoints → notifications(server) → handler → endpoints retain cycle.
    NIAgentEndpoints * endpoints = [NIAgentEndpoints new];
    __weak NIAgentEndpoints * weakEndpoints = endpoints;

    NSString * strategy = NSProcessInfo.processInfo.environment[@"MACCHINA_ESTABLISH_REPLY"] ?: @"echo";
    __block BOOL seenFirstFrame = NO;

    NIRawFrameHandler handler = ^NSData *(NIRawPortServer * server, NSData * frame)
    {
        // The first frame the agent sends here should be the connection
        // completion (MsgConnectionEstablished, tag undecoded — §6). Answer
        // it per MACCHINA_ESTABLISH_REPLY; later frames are input-event
        // notifications — hand them to onInboundFrame and reply nil.
        if (seenFirstFrame)
        {
            void (^sink)(NSData *) = weakEndpoints.onInboundFrame;
            if (sink)
                sink(frame);
            return nil;
        }
        seenFirstFrame = YES;

        NSLog(@"*** [%@] FIRST inbound frame on %@ — MsgConnectionEstablished candidate, "
              @"replying per strategy '%@'", label, server.name, strategy);

        if ([strategy isEqualToString:@"none"])
            return nil;
        if ([strategy isEqualToString:@"empty"])
            return [NSData data];
        if ([strategy isEqualToString:@"true"])
        {
            uint32_t t = 'true';
            return [NSData dataWithBytes:&t length:sizeof(t)];
        }
        return frame;   // echo
    };

    NIRawPortServer * notifications = [NIRawPortServer serverWithName:response.outPortName
                                                              handler:handler];
    if (notifications == nil)
    {
        NSLog(@"[%@] could not create receive port '%@' — name taken "
              @"(stale port from a dead client?)", label, response.outPortName);
        return nil;
    }
    [notifications scheduleInRunLoop:[NSRunLoop currentRunLoop]];

    NSLog(@"[%@] endpoints open — request '%@', receive '%@'",
          label, response.inPortName, response.outPortName);

    endpoints.label         = label;
    endpoints.request       = request;
    endpoints.notifications = notifications;
    return endpoints;
}

- (NSData *)registerNotificationPort:(NIAgentEndpoints *)endpoints;
{
    return [self registerNotificationPortName:endpoints.notifications.name
                                       toPort:endpoints.request];
}

- (NSData *)registerNotificationPortName:(NSString *)portName
                                  toPort:(NIRawPortClient *)requestPort;
{
    NSData * name = [portName dataUsingEncoding:NSASCIIStringEncoding
                           allowLossyConversion:YES];

    NSMutableData * body = [NSMutableData data];
    // Decoded from Maschine 2's NHL2::MessageSetString ctor (LEARNINGS.md E11):
    // the register frame is [msgid 0x03404300][8 ZERO bytes][strlen][name+NUL].
    // The ctor never writes those 8 bytes, so they go out zero — this is the
    // field that binds the receive port to the controller connection. Our old
    // 0xabababab/0xcdcdcdcd "v2 sentinels" here were the likely reason the
    // subscription↔controller +0x40 link never formed.
    AppendU32(body, 0);
    AppendU32(body, 0);
    AppendU32(body, (uint32_t)name.length + 1);
    [body appendData:name];
    [body increaseLengthBy:1];

    return [self sendTag:kNITagSetAsciiString body:body toPort:requestPort];
}

- (NSData *)sendSubscribeVoidOp:(NIAgentEndpoints *)endpoints;
{
    return [self sendTag:kNITagSubscribeVoidOp body:nil toPort:endpoints.request];
}

@end
