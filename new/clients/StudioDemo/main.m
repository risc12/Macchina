//
//  main.m
//  MacchinaHWClient — NewClients/HardwareAgentClient
//
//  A small demo of NIStudioController: connect to the real NIHardwareAgent as a
//  v3 Studio client, then drive the displays + LEDs and print pad/knob/button/
//  wheel input. All the protocol lives in NIStudioController / NIAgentConnection
//  (see docs/protocol.md, LEARNINGS.md E17–E18).
//
//  Usage:   MacchinaHWClient [serial]        (default serial 39195855)
//  Env:
//    (default)             all LEDs "breathe" (brightness ramps up/down) — an
//                          unmissable proof the LED path is live
//    MACCHINA_LED_CHASE=1  walk one lit channel 0→212, ~0.15s each — handy for
//                          mapping channel index → physical LED
//    MACCHINA_LED_ALL=NN   static hex level 00–ff for every LED
//    MACCHINA_NO_DRAW=1    skip the initial display fill
//
//  Needs NIHardwareAgent RUNNING and a Studio plugged in.
//

#import <Foundation/Foundation.h>
#import <math.h>
#import "NIStudioController.h"

// Delegate: just log every input event with its ids/values.
@interface DemoDelegate : NSObject <NIStudioControllerDelegate>
@end

@implementation DemoDelegate

- (void)studioController:(NIStudioController *)c padEvent:(NIStudioPadEvent)e
{
    const char * what = e.state == NIStudioPadHit      ? "hit    " :
                        e.state == NIStudioPadRelease  ? "release" :
                        e.state == NIStudioPadPressure ? "press  " : "?      ";
    NSLog(@"pad %2u  %s  pressure %.3f", e.padId, what, e.pressure);
}

- (void)studioController:(NIStudioController *)c buttonId:(uint32_t)b down:(BOOL)down
{
    NSLog(@"button 0x%02x  %s", b, down ? "DOWN" : "up");
}

- (void)studioController:(NIStudioController *)c knobId:(uint32_t)k delta:(float)d
{
    NSLog(@"knob %u  delta %+.4f", k, d);
}

- (void)studioController:(NIStudioController *)c wheelId:(uint32_t)w step:(int32_t)s
{
    NSLog(@"wheel %u  step %+d", w, s);
}

@end


int main(int argc, const char * argv[])
{
    @autoreleasepool
    {
        NSString * serial = (argc > 1) ? @(argv[1]) : @"39195855";

        NIStudioController * studio = [NIStudioController studioWithSerial:serial];
        DemoDelegate * delegate = [DemoDelegate new];
        studio.delegate = delegate;

        NSLog(@"connecting to Maschine Studio (serial %@)…", serial);
        if ([studio connect] == NO)
        {
            fprintf(stderr, "connect failed — is NIHardwareAgent running and the Studio plugged in?\n");
            return 1;
        }
        NSLog(@"connected + focused. Press pads / buttons / knobs / the wheel to see events.");

        // Displays: fill left red, right blue (unmissable proof of the draw path).
        if (![NSProcessInfo.processInfo.environment[@"MACCHINA_NO_DRAW"] boolValue])
        {
            [studio fillDisplay:0 rgb565:0xF800];   // red
            [studio fillDisplay:1 rgb565:0x001F];   // blue
        }

        // LEDs.
        NSProcessInfo * pi = NSProcessInfo.processInfo;
        NSString * allEnv = pi.environment[@"MACCHINA_LED_ALL"];
        NSString * hexEnv = pi.environment[@"MACCHINA_LED_HEX"];   // 426 hex chars = 213 bytes
        if (hexEnv.length >= 2)
        {
            // Replay a captured raw 213-byte LED buffer verbatim (e.g. M2's Mix state).
            NSMutableData * bytes = [NSMutableData data];
            for (NSUInteger i = 0; i + 1 < hexEnv.length; i += 2)
            {
                unsigned v = 0;
                sscanf([[hexEnv substringWithRange:NSMakeRange(i, 2)] UTF8String], "%x", &v);
                uint8_t b = (uint8_t)v; [bytes appendBytes:&b length:1];
            }
            NSData * snapshot = [bytes copy];
            NSUInteger n = MIN(snapshot.length, NIStudioLEDChannelCount);
            NSLog(@"replaying %lu-byte captured LED buffer verbatim", (unsigned long)n);
            dispatch_queue_t hq = dispatch_queue_create("macchina.hex", DISPATCH_QUEUE_SERIAL);
            dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, hq);
            dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0),
                                      (uint64_t)(1.0 / 30 * NSEC_PER_SEC), (uint64_t)(0.005 * NSEC_PER_SEC));
            dispatch_source_set_event_handler(timer, ^{
                const uint8_t * buf = snapshot.bytes;
                for (NSUInteger i = 0; i < n; i++)
                    [studio setLED:i level:buf[i]];
                [studio flushLEDs];
            });
            dispatch_resume(timer);
        }
        else if ([pi.environment[@"MACCHINA_JOG"] boolValue])
        {
            // Fill the WHOLE jog ring (all 15 segments) — should be a closed ring.
            // Set MACCHINA_JOG_CHASE=1 to instead walk one lit segment around it.
            BOOL chase = [pi.environment[@"MACCHINA_JOG_CHASE"] boolValue];
            dispatch_queue_t jq = dispatch_queue_create("macchina.jog", DISPATCH_QUEUE_SERIAL);
            __block int seg = 0;
            dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, jq);
            dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0),
                                      (uint64_t)((chase ? 0.15 : 0.2) * NSEC_PER_SEC), (uint64_t)(0.01 * NSEC_PER_SEC));
            dispatch_source_set_event_handler(timer, ^{
                [studio setAllLEDs:0];
                if (chase)
                {
                    [studio setJogRingSegment:seg level:127];
                    seg = (seg + 1) % NIStudioJogRingSegments;
                }
                else
                {
                    for (int i = 0; i < NIStudioJogRingSegments; i++)
                        [studio setJogRingSegment:i level:127];
                }
                [studio flushLEDs];
            });
            dispatch_resume(timer);
            NSLog(@"%s the jog ring (%d segments, ch %lu-%lu)", chase ? "chasing" : "filling",
                  NIStudioJogRingSegments, (unsigned long)NIStudioJogRingBase,
                  (unsigned long)(NIStudioJogRingBase + NIStudioJogRingSegments - 1));
        }
        else if ([pi.environment[@"MACCHINA_VU"] boolValue])
        {
            // Bounce both VU meters (left/right out of phase) to verify the ladders.
            dispatch_queue_t vq = dispatch_queue_create("macchina.vu", DISPATCH_QUEUE_SERIAL);
            NSTimeInterval t0 = pi.systemUptime;
            dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, vq);
            dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0),
                                      (uint64_t)(1.0 / 30 * NSEC_PER_SEC), (uint64_t)(0.005 * NSEC_PER_SEC));
            BOOL sweep = [pi.environment[@"MACCHINA_VU_SWEEP"] boolValue];
            dispatch_source_set_event_handler(timer, ^{
                double s = pi.systemUptime - t0;
                [studio setAllLEDs:0];
                if (sweep)
                {
                    // Hold both meters full; ramp the colour channel 0→127 to reveal
                    // every colour the LED_LEVEL_COLOR channel can produce.
                    [studio setVU:YES segments:NIStudioVUSegments];
                    [studio setVU:NO  segments:NIStudioVUSegments];
                    int lvl = (int)fmod(s * 16.0, 128.0);   // ramp ~8s
                    [studio setLED:NIStudioVUColorChannel level:(uint8_t)lvl];
                    static int last = -1;
                    if (lvl / 8 != last) { last = lvl / 8; NSLog(@"LED_LEVEL_COLOR = %d", lvl); }
                }
                else
                {
                    NSUInteger l = (NSUInteger)lround(NIStudioVUSegments * 0.5 * (1 - cos(2*M_PI*s/1.5)));
                    NSUInteger r = (NSUInteger)lround(NIStudioVUSegments * 0.5 * (1 - cos(2*M_PI*s/1.5 + M_PI)));
                    [studio setVU:YES segments:l];
                    [studio setVU:NO  segments:r];
                    [studio setLED:NIStudioVUColorChannel level:(((int)(s / 3.0)) & 1) ? 127 : 0];
                }
                [studio flushLEDs];
            });
            dispatch_resume(timer);
            NSLog(@"bouncing both VU meters");
        }
        else if ([pi.environment[@"MACCHINA_PADS"] boolValue])
        {
            // Paint all 16 pads a rainbow (each a distinct hue) to verify the RGB map.
            dispatch_queue_t pq = dispatch_queue_create("macchina.pads", DISPATCH_QUEUE_SERIAL);
            dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, pq);
            dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0),
                                      (uint64_t)(1.0 / 20 * NSEC_PER_SEC), (uint64_t)(0.005 * NSEC_PER_SEC));
            dispatch_source_set_event_handler(timer, ^{
                [studio setAllLEDs:0];
                for (int i = 0; i < NIStudioPadCount; i++)
                {
                    // Simple hue sweep → RGB (0..127).
                    double h = (double)i / NIStudioPadCount * 6.0;
                    int    s = (int)h; double f = h - s;
                    uint8_t v = 127, p = 0, qy = (uint8_t)(127 * (1 - f)), t = (uint8_t)(127 * f);
                    uint8_t r, g, b;
                    switch (s % 6) {
                        case 0: r=v; g=t; b=p; break;
                        case 1: r=qy;g=v; b=p; break;
                        case 2: r=p; g=v; b=t; break;
                        case 3: r=p; g=qy;b=v; break;
                        case 4: r=t; g=p; b=v; break;
                        default:r=v; g=p; b=qy;break;
                    }
                    [studio setPadLED:i red:r green:g blue:b];
                }
                for (int i = 0; i < NIStudioGroupCount; i++)
                {
                    double h = (double)i / NIStudioGroupCount * 6.0;
                    int    s = (int)h; double f = h - s;
                    uint8_t v = 127, p = 0, qy = (uint8_t)(127 * (1 - f)), t = (uint8_t)(127 * f);
                    uint8_t r, g, b;
                    switch (s % 6) {
                        case 0: r=v; g=t; b=p; break;
                        case 1: r=qy;g=v; b=p; break;
                        case 2: r=p; g=v; b=t; break;
                        case 3: r=p; g=qy;b=v; break;
                        case 4: r=t; g=p; b=v; break;
                        default:r=v; g=p; b=qy;break;
                    }
                    [studio setGroupLED:i red:r green:g blue:b];
                }
                [studio flushLEDs];
            });
            dispatch_resume(timer);
            NSLog(@"painting all 16 pads a rainbow");
        }
        else
        if (allEnv)
        {
            // Continuously re-send a static level (so a one-shot isn't lost).
            uint8_t lvl = (uint8_t)strtoul(allEnv.UTF8String, NULL, 16);
            dispatch_queue_t aq = dispatch_queue_create("macchina.all", DISPATCH_QUEUE_SERIAL);
            dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, aq);
            dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0),
                                      (uint64_t)(1.0 / 30 * NSEC_PER_SEC), (uint64_t)(0.005 * NSEC_PER_SEC));
            dispatch_source_set_event_handler(timer, ^{
                [studio setAllLEDs:lvl];
                [studio flushLEDs];
            });
            dispatch_resume(timer);
            NSLog(@"holding ALL LEDs at level 0x%02x", lvl);
        }
        else if ([pi.environment[@"MACCHINA_LED_CHASE"] boolValue])
        {
            // Walk one lit channel at a time — for mapping channel → physical LED.
            //   MACCHINA_LED_CHASE_FROM / _TO  channel range (default 0 .. 212)
            //   MACCHINA_LED_CHASE_MS          ms per channel (default 400)
            NSString * fromEnv = pi.environment[@"MACCHINA_LED_CHASE_FROM"];
            NSString * toEnv   = pi.environment[@"MACCHINA_LED_CHASE_TO"];
            NSString * msEnv   = pi.environment[@"MACCHINA_LED_CHASE_MS"];
            NSString * wEnv    = pi.environment[@"MACCHINA_LED_CHASE_WIDTH"];
            NSUInteger from = fromEnv ? (NSUInteger)fromEnv.integerValue : 0;
            NSUInteger to   = toEnv   ? (NSUInteger)toEnv.integerValue   : NIStudioLEDChannelCount - 1;
            double     ms   = msEnv   ? msEnv.doubleValue : 400.0;
            NSUInteger width = wEnv ? (NSUInteger)MAX(1, wEnv.integerValue) : 1;

            // Re-send the current block continuously (~30Hz) so the agent keeps
            // pushing it to USB — a one-shot LED message per block doesn't reliably
            // reach the hardware. The block advances every `ms` based on elapsed time.
            NSUInteger span = (to >= from) ? (to - from + 1) : 1;
            NSUInteger nblocks = (span + width - 1) / width;
            dispatch_queue_t cq = dispatch_queue_create("macchina.chase", DISPATCH_QUEUE_SERIAL);
            NSTimeInterval t0 = NSProcessInfo.processInfo.systemUptime;
            __block NSInteger lastBlock = -1;
            dispatch_source_t timer =
                dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, cq);
            dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0),
                                      (uint64_t)(1.0 / 30 * NSEC_PER_SEC), (uint64_t)(0.005 * NSEC_PER_SEC));
            dispatch_source_set_event_handler(timer, ^{
                double secs = NSProcessInfo.processInfo.systemUptime - t0;
                NSUInteger block = ((NSUInteger)(secs * 1000.0 / ms)) % nblocks;
                NSUInteger ch = from + block * width;
                [studio setAllLEDs:0];   // 0 = off
                for (NSUInteger i = 0; i < width && ch + i <= to; i++)
                    [studio setLED:ch + i level:127];   // 7-bit full
                [studio flushLEDs];
                if ((NSInteger)block != lastBlock)
                {
                    NSLog(@"LED chase: channels %lu–%lu", (unsigned long)ch,
                          (unsigned long)MIN(ch + width - 1, to));
                    lastBlock = (NSInteger)block;
                }
            });
            dispatch_resume(timer);
        }
        else
        {
            // Breathe: every LED's brightness follows a smooth cosine. Runs on its
            // OWN serial queue so the (blocking) LED sends don't fight the main
            // run loop that's busy decoding your key presses — that contention was
            // what made it flash. Time-based phase, so brightness stays a smooth
            // function of wall-clock even if a frame is late.
            const double period = 2.4;   // seconds per breath
            dispatch_queue_t q = dispatch_queue_create("macchina.breath", DISPATCH_QUEUE_SERIAL);
            NSTimeInterval t0 = NSProcessInfo.processInfo.systemUptime;   // real monotonic seconds
            dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
            dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0),
                                      (uint64_t)(1.0 / 60 * NSEC_PER_SEC), (uint64_t)(0.002 * NSEC_PER_SEC));
            dispatch_source_set_event_handler(timer, ^{
                // Brightness is a function of real elapsed wall-clock time, so the
                // 2.4s period holds even if the timer fires slower than 60Hz.
                double secs = NSProcessInfo.processInfo.systemUptime - t0;
                double phase = 2.0 * M_PI * secs / period;
                // LED intensity appears to be 7-bit (0–127): bit 7 isn't brightness,
                // so a 0–255 ramp wraps to dark at 128. Stay in 0–127.
                uint8_t level = (uint8_t)lround(63.5 * (1.0 - cos(phase)));   // 0→127→0
                [studio setAllLEDs:level];
                [studio flushLEDs];
            });
            dispatch_resume(timer);
        }

        [[NSRunLoop currentRunLoop] run];
    }
    return 0;
}
