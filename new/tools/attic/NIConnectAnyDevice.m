//
//  NIConnectAnyDevice.m
//  Macchina — NITooling
//

#import "NIConnectAnyDevice.h"
#import "NIProtocolMessages.h"

@implementation NIMainHandlerServer (ConnectAnyDevice)

- (NSData *)handleNIDeviceConnectMessage:(NIDeviceConnectMessage *)message
{
    NSLog(@"    connect from '%@' for controllerId 0x%08x (role %08x, boh %08x) — accepting",
          message.clientName, message.controllerId, message.clientRole, message.boh);

    // Same reply layout as the stock handler, same stub port names — the client
    // uses whatever names we hand back, so the 0001 names work for any device.
    char        * inPortName  = "NIHWMaschineController0001Request";
    uint32_t      inPortLen   = (uint32_t)strlen(inPortName);

    char        * outPortName = "NIHWMaschineController0001Notification";
    uint32_t      outPortLen  = (uint32_t)strlen(outPortName);

    uint32_t      replyLen    = 0x0c + inPortLen + outPortLen + 2;

    uint8_t reply[replyLen];
    memset(reply, 0, replyLen);

    *((uint32_t *)(reply + 0x00)) = 'true';
    *((uint32_t *)(reply + 0x04)) = inPortLen + 1;
    *((uint32_t *)(reply + 0x08 + inPortLen + 1)) = outPortLen + 1;

    memcpy(reply + 0x08,                 inPortName,  inPortLen);
    memcpy(reply + 0x0c + inPortLen + 1, outPortName, outPortLen);

    return [NSData dataWithBytes:reply length:replyLen];
}

@end
