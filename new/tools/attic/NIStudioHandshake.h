//
//  NIStudioHandshake.h
//  Macchina — NITooling
//
//  Handlers for the v3-era main-handler messages observed live from Maschine 2
//  (2026-07 capture): it polls `0x03536756` (GetServiceVersion, protocol-v3
//  prefix) and `0x02447500` ("Du" — device-presence poll cycling through USB
//  product ids, carrying the client identity 'NiM2'/'prmy'). The stock 2012
//  stub answers neither, so Maschine 2 loops in discovery forever.
//
//  Implemented as a handleNIUnknownMessage: category so NICommon stays
//  untouched; ids not yet in NIMessage's table arrive here as NIUnknownMessage.
//

#import "NIMainHandlerServer.h"
#import "NIMessage.h"

// USB product id our stub claims is present (Maschine Studio).
#define kNIStudioProductId 0x1300

@interface NIMainHandlerServer (StudioHandshake)
- (NSData *)handleNIUnknownMessage:(NIMessage *)message;
@end
