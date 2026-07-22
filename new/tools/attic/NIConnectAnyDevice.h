//
//  NIConnectAnyDevice.h
//  Macchina — NITooling
//
//  NIMainHandlerServer's stock connect handler rejects any controllerId other
//  than 0x0808 (Maschine MK1) — so NI software announcing a different device
//  (e.g. Maschine Studio, 0x1300) gets 'fail' and hangs up before sending the
//  traffic we want to capture. This category replaces the handler with one that
//  accepts ANY controllerId, logs it, and hands out the same stub port names.
//
//  Category method replacement, tooling-only: linked into server-tap builds,
//  absent from the stock MacchinaServer.
//

#import "NIMainHandlerServer.h"

@interface NIMainHandlerServer (ConnectAnyDevice)
@end
