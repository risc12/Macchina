//
//  NICaptureTap.m
//  Macchina — NITooling
//

#import "NICaptureTap.h"
#import "NIHexDump.h"
#import "NIServer.h"
#import <objc/runtime.h>

// -handleIncomingData: and -name are on NIServer but not in its public header;
// declare what we call so the compiler is happy under ARC.
@interface NIServer (NICaptureTapPrivate)
- (NSData *)handleIncomingData:(NSData *)data;
- (NSString *)name;
@end

static NSString              * gCaptureDir    = nil;
static NSMutableDictionary   * gFileHandles   = nil;  // port name -> NSFileHandle

@implementation NICaptureTap

+ (BOOL)installToDirectory:(NSString *)dir
{
    static BOOL installed = NO;
    if (installed)
        return YES;

    NSError * err = nil;
    if (![[NSFileManager defaultManager] createDirectoryAtPath:dir
                                   withIntermediateDirectories:YES
                                                    attributes:nil
                                                         error:&err])
    {
        NSLog(@"NICaptureTap: cannot create %@: %@", dir, err);
        return NO;
    }

    gCaptureDir  = [dir copy];
    gFileHandles = [NSMutableDictionary dictionary];

    Method original = class_getInstanceMethod([NIServer class], @selector(handleIncomingData:));
    Method replacement = class_getInstanceMethod([NICaptureTap class], @selector(nitap_handleIncomingData:));
    // Move our IMP onto NIServer under a fresh selector, then swap.
    class_addMethod([NIServer class],
                    @selector(nitap_handleIncomingData:),
                    method_getImplementation(replacement),
                    method_getTypeEncoding(replacement));
    Method installed_replacement = class_getInstanceMethod([NIServer class], @selector(nitap_handleIncomingData:));
    method_exchangeImplementations(original, installed_replacement);

    installed = YES;
    NSLog(@"NICaptureTap: capturing raw frames to %@", dir);
    return YES;
}

+ (void)load
{
    const char * dir = getenv("MACCHINA_CAPTURE_DIR");
    if (dir != NULL)
        [NICaptureTap installToDirectory:[NSString stringWithUTF8String:dir]];
}

// This method is added to NIServer; after the swap, `self` is an NIServer and
// the original implementation is reachable via this same selector.
- (NSData *)nitap_handleIncomingData:(NSData *)data
{
    NIServer * server = (NIServer *)self;
    NSString * port   = [server name] ?: @"unknown";

    NSLog(@"[capture %@] %@", port, NIDescribeFrame(data));

    @synchronized (gFileHandles)
    {
        NSFileHandle * fh = gFileHandles[port];
        if (fh == nil)
        {
            NSString * path = [gCaptureDir stringByAppendingPathComponent:
                               [NSString stringWithFormat:@"capture-%@.bin", port]];
            [[NSFileManager defaultManager] createFileAtPath:path contents:nil attributes:nil];
            fh = [NSFileHandle fileHandleForWritingAtPath:path];
            if (fh != nil)
                gFileHandles[port] = fh;
        }

        if (fh != nil && data != nil)
        {
            uint32_t len = (uint32_t)data.length;
            [fh writeData:[NSData dataWithBytes:&len length:sizeof(len)]];
            [fh writeData:data];
        }
    }

    // Call through to the real handler (swapped onto this selector).
    return [self nitap_handleIncomingData:data];
}

@end
