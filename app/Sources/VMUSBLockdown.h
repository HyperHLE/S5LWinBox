// Paired, bounded guest services. Worker-thread only; no guest disk access.
#import <Foundation/Foundation.h>
@class VMUSBTransport, VMUSBService;
@interface VMUSBLockdown : NSObject
+ (instancetype)openWithTransport:(VMUSBTransport *)transport instanceID:(NSString *)identifier
                         canceled:(BOOL (^)(void))canceled deadline:(NSTimeInterval)deadline
                            error:(NSError **)error;
// Wrap an already connected service. Pairing is established by openWithTransport.
- (instancetype)initWithService:(VMUSBService *)service;
- (NSDictionary *)request:(NSString *)request values:(NSDictionary *)values error:(NSError **)error;
- (VMUSBService *)openService:(NSString *)name error:(NSError **)error;
// Unix UTC seconds only: never change timezone, uptime, or emulator clocks.
// Sample now after the guest GetValue response, not before connecting/pairing.
- (BOOL)synchronizeClockWithNow:(NSTimeInterval (^)(void))now error:(NSError **)error;
// Use the guest's timezone setter, never a UTC adjustment or a live disk edit.
// Confirm the named zone and its current offset; old firmware may lack new zones.
- (BOOL)synchronizeTimeZone:(NSString *)identifier now:(NSTimeInterval (^)(void))now error:(NSError **)error;
- (void)close;
@end
