//
//  NIRequestPortCapture.h
//  Macchina — NITooling
//
//  On the request port, Maschine 2's v3 messages arrive with the 0x03 protocol
//  prefix, so they don't match the message-id table and land as NIUnknownMessage
//  — bypassing the stock class-dispatched handlers entirely. This category adds
//  a tag-dispatching handleNIUnknownMessage: to NIControllerRequestServer that:
//    * 0xNN404300 (SetAsciiString / register notification port) — stands up the
//      notification client back to Maschine 2 (triggers the agent's puppa:
//      deviceStateChange+setFocus, i.e. "device claimed & focused") and ACKs.
//    * 0xNN647344 (DisplayDraw) — routes to the capture handler (hexdump + PGM).
//    * anything else — logs the raw frame and ACKs 'true' so the app proceeds.
//

#import "NIControllerRequestServer.h"
#import "NIMessage.h"

@interface NIControllerRequestServer (RequestPortCapture)
- (NSData *)handleNIUnknownMessage:(NIMessage *)message;
@end
