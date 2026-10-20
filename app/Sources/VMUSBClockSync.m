// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMUSBClockSync.h"
#import "VMUSBLockdown.h"
#import <dispatch/dispatch.h>
@interface VMUSBClockSync ()
@property (atomic) BOOL finished;
@property (atomic) BOOL succeeded;
@property (atomic, copy) NSString *message;
@end
@implementation VMUSBClockSync {
    VMUSBTransport *_transport;
    NSString *_identifier;
    BOOL _started;
}
- (instancetype)initWithTransport:(VMUSBTransport *)transport instanceID:(NSString *)identifier {
    self = [super init]; if (self) { _transport = transport; _identifier = [identifier copy]; } return self;
}
- (void)start {
    @synchronized(self) { if (_started) return; _started = YES; }
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool {
            NSError *error = nil;
            VMUSBLockdown *client = [VMUSBLockdown openWithTransport:self->_transport instanceID:self->_identifier
                canceled:^BOOL{ return self.canceled; }
                deadline:NSProcessInfo.processInfo.systemUptime + 45 error:&error];
            BOOL calendarOK = client && [client synchronizeClockWithNow:^NSTimeInterval{
                return NSDate.date.timeIntervalSince1970;
            } error:&error];
            // Obtain the system zone after pairing, not when the task was queued.
            // systemTimeZone follows the host setting, not an app-local override.
            // Foundation caches it until explicitly reset (including on resume).
            [NSTimeZone resetSystemTimeZone];
            NSString *zone = NSTimeZone.systemTimeZone.name;
            self.succeeded = calendarOK && [client synchronizeTimeZone:zone now:^NSTimeInterval{
                return NSDate.date.timeIntervalSince1970;
            } error:&error];
            [client close];
            self.message = self.succeeded ? [NSString stringWithFormat:@"guest calendar synchronized; timezone %@", zone] :
                calendarOK ? [NSString stringWithFormat:@"guest calendar synchronized; timezone sync unavailable: %@",
                    error.localizedDescription ?: @"no confirmation"] :
                error.localizedDescription ?: @"guest calendar sync unavailable";
            self.finished = YES; // Publish the result last.
        }
    });
}
@end
