//
//  NIRequestPortCapture.m
//  Macchina — NITooling
//

#import "NIRequestPortCapture.h"
#import "NIDisplayDrawCapture.h"   // handleNIDisplayDrawMessage: + inbound parse
#import "NIProtocolMessages.h"
#import "NIHexDump.h"
#import "NIAgent.h"
#import "NIClient.h"
#import "NIMessageBases.h"

static NSData * AckTrue(void)
{
    uint32_t theTrue = 'true';
    return [NSData dataWithBytes:&theTrue length:sizeof(theTrue)];
}

// Take a v2 message's wire bytes and re-stamp the protocol prefix to 0x03.
// Maschine 2, once it negotiates v3, ignores v2-prefixed notifications — so the
// agent's puppa (v2 deviceStateChange/setFocus) never lands. This sends the v3
// form the real agent uses (observed live: 0x03444e00 on the notification port).
static void SendV3Notification(NIClient * client, NIMessage * v2msg)
{
    NSMutableData * d = [[v2msg dataRepresentation] mutableCopy];
    if (d.length < 4) return;
    ((uint8_t *)d.mutableBytes)[3] = 0x03;                 // 0x02?????? -> 0x03??????
    [client sendMessage:(NIUnknownMessage *)[NIUnknownMessage messageFromData:d]];
}

@implementation NIControllerRequestServer (RequestPortCapture)

- (NSData *)handleNIUnknownMessage:(NIMessage *)message
{
    NSData   * raw = [message dataRepresentation];
    uint32_t   tag = message.messageId & 0x00ffffff;

    switch (tag)
    {
        case 0x404300: // SetAsciiString — register the client's notification port
        {
            NSString * name = nil;
            if (raw.length > 0x10)
                name = [NSString stringWithCString:(const char *)raw.bytes + 0x10
                                          encoding:NSUTF8StringEncoding];
            NSLog(@"    v3 register notification port: %@", name);
            if (name.length)
            {
                [self.agent createNotificationClientWithName:name];  // also schedules v2 puppa
                // Push the v3 claim immediately — device present + focused.
                NIClient * notif = self.agent.controllerNotificationClient;
                SendV3Notification(notif, [NIDeviceStateChangeMessage messageWithBool:YES]);
                SendV3Notification(notif, [NISetFocusMessage messageWithBool:YES]);
                NSLog(@"    pushed v3 deviceStateChange(true) + setFocus(true)");
            }
            return AckTrue();
        }

        case 0x447143: // subscriber "send current state" (IPCServer::onSubscriberMessage).
        {                // Real agent replies EMPTY and PUSHES device state to the
                         // subscriber. We pushed too early (on 0x404300) before the app
                         // subscribed; push again here, in response to the snapshot request.
            NIClient * notif = self.agent.controllerNotificationClient;
            if (notif)
            {
                SendV3Notification(notif, [NIDeviceStateChangeMessage messageWithBool:YES]);
                SendV3Notification(notif, [NISetFocusMessage messageWithBool:YES]);
                NSLog(@"    v3 0x447143 (subscribe/snapshot) -> pushed state, empty ack");
            }
            return [NSData data];
        }

        case 0x647344: // DisplayDraw — the prize; parse via the inbound category
        {
            NIDisplayDrawMessage * m = (NIDisplayDrawMessage *)[NIDisplayDrawMessage messageFromData:raw];
            if (m)
                return [self handleNIDisplayDrawMessage:m];
            NSLog(@"    display draw parse failed:\n%@", NIDescribeFrame(raw));
            return AckTrue();
        }

        default:
            NSLog(@"    v3 request tag 0x%06x (unhandled, acking):\n%@", tag, NIDescribeFrame(raw));
            return AckTrue();
    }
}

@end
