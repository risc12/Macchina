//
//  NIDisplayDrawCapture.h
//  Macchina — NITooling
//
//  Makes the *server* side able to receive display draws, which it can't today:
//  NIDisplayDrawMessage ships a +dataRepresentation (client → agent) but no
//  +messageFromData:, so an inbound draw falls through to NIMessage's default
//  which re-dispatches to the same class — infinite recursion. And even parsed,
//  NIControllerRequestServer has no handler, so the frame is lost and the payload
//  (the whole point) is never shown.
//
//  Both fixes live here as categories so NICommon stays untouched:
//    * NIDisplayDrawMessage (Inbound)      — parse the wire format.
//    * NIControllerRequestServer (Capture) — hexdump the payload, dump a PGM,
//                                            and return a plausible ACK.
//
//  If the env var MACCHINA_CAPTURE_DIR is set, each draw's decoded image is
//  written there as a PGM for eyeballing.
//

#import "NIProtocolMessages.h"
#import "NIControllerRequestServer.h"

@interface NIDisplayDrawMessage (Inbound)
+ (NIMessage *)messageFromData:(NSData *)data;
@end

@interface NIControllerRequestServer (Capture)
- (NSData *)handleNIDisplayDrawMessage:(NIDisplayDrawMessage *)message;
@end
