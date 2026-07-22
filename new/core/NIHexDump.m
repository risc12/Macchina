//
//  NIHexDump.m
//  Macchina — NITooling
//

#import "NIHexDump.h"

NSString * NIHexDump(NSData * data)
{
    if (data == nil)
        return @"(nil)";

    const uint8_t * bytes = data.bytes;
    NSUInteger      len    = data.length;

    NSMutableString * out = [NSMutableString stringWithCapacity:len * 4];

    for (NSUInteger row = 0; row < len; row += 16)
    {
        [out appendFormat:@"%08lx  ", (unsigned long)row];

        // Hex columns, split into two groups of 8 for readability.
        for (NSUInteger col = 0; col < 16; col++)
        {
            if (row + col < len)
                [out appendFormat:@"%02x ", bytes[row + col]];
            else
                [out appendString:@"   "];

            if (col == 7)
                [out appendString:@" "];
        }

        // ASCII gutter.
        [out appendString:@" |"];
        for (NSUInteger col = 0; col < 16 && row + col < len; col++)
        {
            uint8_t c = bytes[row + col];
            [out appendFormat:@"%c", (c >= 0x20 && c < 0x7f) ? c : '.'];
        }
        [out appendString:@"|\n"];
    }

    return out;
}

NSString * NIDescribeFrame(NSData * data)
{
    if (data.length < 4)
        return [NSString stringWithFormat:@"(short frame, %lu bytes)\n%@",
                (unsigned long)data.length, NIHexDump(data)];

    uint32_t messageId;
    memcpy(&messageId, data.bytes, sizeof(messageId));

    // Little-endian message-id tag as a 4CC (printable bytes), for eyeballing.
    char tag[5] = {0};
    for (int i = 0; i < 4; i++)
    {
        uint8_t c = (uint8_t)(messageId >> (i * 8));
        tag[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }

    return [NSString stringWithFormat:@"%08x  '%s'  (%lu bytes)\n%@",
            messageId, tag, (unsigned long)data.length, NIHexDump(data)];
}
