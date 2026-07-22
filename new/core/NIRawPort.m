//
//  NIRawPort.m
//  Macchina — NewClients/Shared
//

#import "NIRawPort.h"
#import "NIHexDump.h"

// Display draws are ~256 KiB; dumping them in full would drown the log.
static NSString * DescribeFrameBounded(NSData * frame)
{
    static const NSUInteger kMaxDumpedBytes = 128;

    if (frame == nil)
        return @"(nil)";
    if (frame.length == 0)
        return @"(empty)";
    if (frame.length <= kMaxDumpedBytes)
        return NIDescribeFrame(frame);

    return [NSString stringWithFormat:@"%@         … (%lu bytes total, first %lu shown)",
            NIDescribeFrame([frame subdataWithRange:NSMakeRange(0, kMaxDumpedBytes)]),
            (unsigned long)frame.length,
            (unsigned long)kMaxDumpedBytes];
}


@interface NIRawPortClient ()
@property (readwrite) NSString * name;
@end

@implementation NIRawPortClient
{
    CFMessagePortRef port;
}

+ (instancetype)clientWithName:(NSString *)name;
{
    CFMessagePortRef port = CFMessagePortCreateRemote(NULL, (__bridge CFStringRef)name);
    if (port == NULL)
        return nil;

    NIRawPortClient * zelf = [self new];
    zelf.name  = name;
    zelf->port = port;
    return zelf;
}

- (void)dealloc;
{
    if (port)
        CFRelease(port);
}

- (NSData *)sendFrame:(NSData *)frame;
{
    if (frame.length < 4)
    {
        NSLog(@"%@ !! refusing to send %lu-byte frame (no messageId)", self.name, (unsigned long)frame.length);
        return nil;
    }

    SInt32 msgid;
    memcpy(&msgid, frame.bytes, sizeof(msgid));

    if (!self.quiet)
        NSLog(@"%@ -> %@", self.name, DescribeFrameBounded(frame));

    CFDataRef replyData = NULL;
    SInt32    err       = CFMessagePortSendRequest(port,
                                                   msgid,
                                                   (__bridge CFDataRef)frame,
                                                   2.0,
                                                   2.0,
                                                   kCFRunLoopDefaultMode,
                                                   &replyData);
    NSData * reply = CFBridgingRelease(replyData);

    if (err != kCFMessagePortSuccess)
        NSLog(@"%@ !! CFMessagePortSendRequest error %d", self.name, (int)err);

    if (!self.quiet)
    {
        NSLog(@"%@ <- %@", self.name, reply.length ? NIHexDump(reply) : (reply ? @"(empty)" : @"(no reply)"));
        NSLog(@" ");
    }

    return reply;
}

- (void)postFrame:(NSData *)frame;
{
    if (frame.length < 4)
    {
        NSLog(@"%@ !! refusing to post %lu-byte frame (no messageId)", self.name, (unsigned long)frame.length);
        return;
    }

    SInt32 msgid;
    memcpy(&msgid, frame.bytes, sizeof(msgid));

    if (!self.quiet)
        NSLog(@"%@ ~> %@", self.name, DescribeFrameBounded(frame));

    // replyMode NULL + no return-data pointer → send without waiting for a reply.
    SInt32 err = CFMessagePortSendRequest(port,
                                          msgid,
                                          (__bridge CFDataRef)frame,
                                          2.0,
                                          0.0,
                                          NULL,
                                          NULL);
    if (err != kCFMessagePortSuccess)
        NSLog(@"%@ !! CFMessagePortSendRequest (post) error %d", self.name, (int)err);
}

@end


static CFDataRef RawServerCallback(CFMessagePortRef local, SInt32 msgid, CFDataRef data, void * info);

@interface NIRawPortServer ()
@property (readwrite) NSString         * name;
@property (copy)      NIRawFrameHandler  handler;
@end

@implementation NIRawPortServer
{
    CFMessagePortRef   port;
    CFRunLoopSourceRef source;
}

+ (instancetype)serverWithName:(NSString *)name handler:(NIRawFrameHandler)handler;
{
    NIRawPortServer * zelf = [self new];
    zelf.name    = name;
    zelf.handler = handler;

    CFMessagePortContext context;
    memset(&context, 0, sizeof(context));
    context.info = (__bridge void *)zelf;

    zelf->port = CFMessagePortCreateLocal(NULL,
                                          (__bridge CFStringRef)name,
                                          RawServerCallback,
                                          &context,
                                          NULL);
    if (zelf->port == NULL)
        return nil;

    return zelf;
}

- (void)dealloc;
{
    if (source)
    {
        CFRunLoopSourceInvalidate(source);
        CFRelease(source);
    }
    if (port)
    {
        CFMessagePortInvalidate(port);
        CFRelease(port);
    }
}

- (void)scheduleInRunLoop:(NSRunLoop *)runloop;
{
    source = CFMessagePortCreateRunLoopSource(NULL, port, 0);
    CFRunLoopAddSource([runloop getCFRunLoop], source, kCFRunLoopCommonModes);
}

- (NSData *)handleFrame:(NSData *)frame;
{
    @autoreleasepool
    {
        if (!self.quiet)
            NSLog(@"%@ <- %@", self.name, DescribeFrameBounded(frame));

        NSData * reply = self.handler ? self.handler(self, frame) : nil;

        if (!self.quiet)
        {
            NSLog(@"%@ -> %@", self.name, reply.length ? NIHexDump(reply) : (reply ? @"(empty)" : @"(nil)"));
            NSLog(@" ");
        }

        return reply;
    }
}

@end

static CFDataRef RawServerCallback(CFMessagePortRef local, SInt32 msgid, CFDataRef data, void * info)
{
    NIRawPortServer * server = (__bridge NIRawPortServer *)info;
    return CFBridgingRetain([server handleFrame:(__bridge NSData *)data]);
}
