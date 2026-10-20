// Worker-thread, bounded service I/O. MIT licensed.
#import <Foundation/Foundation.h>
#import <Security/Security.h>
@class VMUSBTransport;
@interface VMUSBService : NSObject
- (instancetype)initWithTransport:(VMUSBTransport *)transport port:(uint16_t)port error:(NSError **)error;
- (instancetype)initWithTransport:(VMUSBTransport *)transport port:(uint16_t)port
                         canceled:(BOOL (^)(void))canceled error:(NSError **)error;
/* Absolute systemUptime deadline, including connect and all subsequent I/O.
 * Zero retains the normal per-operation timeout. Never use cancellation as a timer. */
- (instancetype)initWithTransport:(VMUSBTransport *)transport port:(uint16_t)port
                         canceled:(BOOL (^)(void))canceled deadline:(NSTimeInterval)deadline
                            error:(NSError **)error;
@property (nonatomic, copy) BOOL (^canceled)(void);
// Worker-thread only. Start a separately bounded transfer after a short connect.
- (void)setOperationDeadline:(NSTimeInterval)deadline;
- (BOOL)writeData:(NSData *)data error:(NSError **)error;
- (NSData *)readCount:(NSUInteger)count error:(NSError **)error;
- (BOOL)sendPlist:(NSDictionary *)plist error:(NSError **)error;
- (NSDictionary *)receivePlist:(NSError **)error;
- (NSDictionary *)request:(NSDictionary *)plist error:(NSError **)error;
/* Legacy TLS is confined to this local emulated-device stream. The peer key
 * must equal the guest key used to construct the pairing record. */
- (BOOL)enableTLSWithIdentity:(SecIdentityRef)identity
                 expectedKey:(SecKeyRef)key error:(NSError **)error;
- (void)close;
@end
