//
//  main.m
//  MacchinaLedPoke — NewClients/LedPoke
//
//  Direct interactive LED poker for the Maschine Studio: type a channel number
//  to toggle it, or "channel level" to set a specific brightness. Lit channels
//  accumulate, so you can build up and compare patterns by hand. The 213-byte
//  state is re-sent continuously at 30Hz (a one-shot write isn't reliably pushed).
//
//  Build:  ./build.sh ledpoke
//  Run:    ./build/MacchinaLedPoke [serial]     (NIHardwareAgent must be running)
//
//  Commands (press Enter after each):
//    <ch>            toggle channel <ch> on/off (on = current brightness)
//    <ch> <level>    set channel <ch> to <level> (0–127; 0 = off)
//    b <level>       set the toggle brightness (default 127)
//    all [<level>]   set every channel (default: brightness)
//    off | c         all channels off
//    ?               list currently-lit channels
//    q               quit
//
//  Levels: 1..127 (7-bit). 0 means "off" here (mapped to 1 on the wire, since a
//  raw 0 = "no change"). Channel range 0..212.
//

#import <Foundation/Foundation.h>
#import "NIStudioController.h"

int main(int argc, const char * argv[])
{
    @autoreleasepool
    {
        NSString * serial = (argc > 1) ? @(argv[1]) : @"39195855";
        const int N = (int)NIStudioLEDChannelCount;

        NIStudioController * studio = [NIStudioController studioWithSerial:serial];
        printf("connecting to Maschine Studio (serial %s)…\n", serial.UTF8String);
        if ([studio connect] == NO)
        {
            fprintf(stderr, "connect failed — is NIHardwareAgent running and the Studio plugged in?\n");
            return 1;
        }

        uint8_t * levels = calloc((size_t)N, 1);       // start all at 0
        __block int brightness = 127;

        // Continuous re-send on its own queue; also the ONLY writer of `levels`
        // (stdin dispatches mutations here) so there are no races.
        dispatch_queue_t q = dispatch_queue_create("macchina.ledpoke", DISPATCH_QUEUE_SERIAL);
        dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
        dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0),
                                  (uint64_t)(1.0 / 30 * NSEC_PER_SEC), (uint64_t)(0.005 * NSEC_PER_SEC));
        dispatch_source_set_event_handler(timer, ^{
            for (int i = 0; i < N; i++)
                [studio setLED:(NSUInteger)i level:levels[i]];
            [studio flushLEDs];
        });
        dispatch_resume(timer);

        printf("\nLED poke. %d channels (0–%d).\n"
               "  <ch> = toggle   <ch> <lvl> = set   b <lvl> = brightness\n"
               "  all [<lvl>] = set all   off/c = clear   ? = list lit   q = quit\n\n> ",
               N, N - 1);
        fflush(stdout);

        void (^listLit)(void) = ^{
            NSMutableArray * lit = [NSMutableArray array];
            for (int i = 0; i < N; i++)
                if (levels[i] > 0) [lit addObject:[NSString stringWithFormat:@"%d=%d", i, levels[i]]];
            printf("  lit: %s\n", lit.count ? [lit componentsJoinedByString:@" "].UTF8String : "(none)");
        };

        void (^process)(NSString *) = ^(NSString * raw){
            NSString * line = [raw stringByTrimmingCharactersInSet:
                               [NSCharacterSet whitespaceAndNewlineCharacterSet]];

            if ([line isEqualToString:@"q"])
            {
                dispatch_sync(q, ^{ [studio setAllLEDs:0]; [studio flushLEDs]; });
                printf("bye.\n"); fflush(stdout);
                exit(0);
            }
            else if (line.length == 0)
            {
                // ignore
            }
            else if ([line isEqualToString:@"off"] || [line isEqualToString:@"c"])
            {
                dispatch_sync(q, ^{ memset(levels, 0, (size_t)N); });
                printf("  all 0\n");
            }
            else if ([line isEqualToString:@"?"])
            {
                dispatch_sync(q, ^{ listLit(); });
            }
            else if ([line hasPrefix:@"b"])
            {
                int b = [line substringFromIndex:1].intValue;
                if (b >= 0 && b <= 255) { brightness = b; printf("  brightness = %d\n", b); }
                else printf("  ? brightness 0–255\n");
            }
            else if ([line hasPrefix:@"all"])
            {
                NSString * rest = [[line substringFromIndex:3]
                                   stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
                int lvl = rest.length ? rest.intValue : brightness;
                if (lvl < 0) lvl = 0; if (lvl > 255) lvl = 255;
                dispatch_sync(q, ^{ memset(levels, (uint8_t)lvl, (size_t)N); });
                printf("  all = %d\n", lvl);
            }
            else
            {
                int ch = -1, lvl = -1;
                int got = sscanf(line.UTF8String, "%d %d", &ch, &lvl);
                if (got >= 1 && ch >= 0 && ch < N)
                {
                    dispatch_sync(q, ^{
                        if (got == 2)
                            levels[ch] = (uint8_t)(lvl < 0 ? 0 : (lvl > 255 ? 255 : lvl));   // set (raw 0..255)
                        else
                            levels[ch] = (levels[ch] > 0) ? 0 : (uint8_t)brightness;         // toggle (off = 0)
                        printf("  ch %d = %d\n", ch, levels[ch]);
                    });
                }
                else
                {
                    printf("  ? (<ch> | <ch> <lvl> | b <lvl> | all | off | ? | q)\n");
                }
            }
            printf("> "); fflush(stdout);
        };

        dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
            char buf[256];
            while (fgets(buf, sizeof(buf), stdin) != NULL)
            {
                NSString * s = @(buf);
                dispatch_async(dispatch_get_main_queue(), ^{ process(s); });
            }
            dispatch_async(dispatch_get_main_queue(), ^{ process(@"q"); });
        });

        [[NSRunLoop currentRunLoop] run];
        free(levels);
    }
    return 0;
}
