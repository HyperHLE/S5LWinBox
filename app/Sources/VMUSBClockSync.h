// One cancellable background attempt. No engine or guest-memory ownership.
#import <Foundation/Foundation.h>
@class VMUSBTransport;
@interface VMUSBClockSync : NSObject
@property (atomic) BOOL canceled;
@property (atomic, readonly) BOOL finished;
@property (atomic, readonly) BOOL succeeded;
@property (atomic, copy, readonly) NSString *message;
- (instancetype)initWithTransport:(VMUSBTransport *)transport instanceID:(NSString *)identifier;
- (void)start;
@end
