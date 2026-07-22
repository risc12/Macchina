//
//  NIReplay.m
//  Macchina — NITooling
//

#import "NIReplay.h"
#import "NIHexDump.h"
#import "NIClient.h"
#import "NIMessageBases.h"

NSArray<NSData *> * NIReadCaptureFile(NSString * path)
{
    NSData * blob = [NSData dataWithContentsOfFile:path];
    if (blob == nil)
        return nil;

    NSMutableArray * frames = [NSMutableArray array];
    const uint8_t * bytes = blob.bytes;
    NSUInteger      total = blob.length;
    NSUInteger      pos   = 0;

    while (pos + sizeof(uint32_t) <= total)
    {
        uint32_t len = *(const uint32_t *)(bytes + pos);
        pos += sizeof(uint32_t);

        if (pos + len > total)  // truncated tail — stop cleanly
            break;

        [frames addObject:[NSData dataWithBytes:bytes + pos length:len]];
        pos += len;
    }

    return frames;
}

void NIDescribeCaptureFile(NSString * path)
{
    NSArray<NSData *> * frames = NIReadCaptureFile(path);
    if (frames == nil)
    {
        NSLog(@"NIReplay: cannot read %@", path);
        return;
    }

    NSLog(@"NIReplay: %lu frame(s) in %@", (unsigned long)frames.count, path);
    NSUInteger i = 0;
    for (NSData * frame in frames)
        NSLog(@"--- frame %lu ---\n%@", (unsigned long)i++, NIDescribeFrame(frame));
}

NSUInteger NISendFramesToPort(NSArray<NSData *> * frames, NSString * portName)
{
    NIClient * client = [[NIClient alloc] initWithName:portName];
    NSUInteger acked  = 0;

    for (NSData * frame in frames)
    {
        NIUnknownMessage * raw = (NIUnknownMessage *)[NIUnknownMessage messageFromData:frame];
        NSData * reply = [client sendMessage:raw];
        if (reply != nil)
            acked++;
    }

    return acked;
}
