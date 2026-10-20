#import <Foundation/Foundation.h>
@class VMUSBTransport;
typedef void (^VMPackageSetupProgress)(NSString *stage, double fraction);
@interface VMPackageBridge : NSObject
/* Local preflight only: a capability does not prove that the guest is ready. */
+ (BOOL)hasCapabilityForInstance:(NSString *)identifier;
/* First live connection after stopped setup, independent of navigation. This
 * is a host UI hint, not the durable disk-publication pending marker. */
+ (BOOL)awaitingFirstConnectionForInstance:(NSString *)identifier;
+ (void)noteSuccessfulConnectionForInstance:(NSString *)identifier;
- (instancetype)initWithTransport:(VMUSBTransport *)transport instanceID:(NSString *)identifier;
@property (atomic) BOOL canceled;
@property (nonatomic, copy) void (^progress)(NSString *text);
// Cydia finish priority: 0 return, 1 reopen, 2 restart, 3 reload, 4 reboot.
@property (atomic, readonly) NSUInteger finishAction;
@property (atomic, readonly) BOOL finishReady;
// The helper is reachable but its first-boot finalizer is not done yet.
@property (atomic, readonly) BOOL setupPending;
@property (nonatomic) BOOL respringAfterChanges;
// Worker only. No raw guest disk access in any live operation.
- (NSData *)status:(NSError **)error;
- (NSData *)install:(NSArray<NSURL *> *)archives status:(NSData *)status error:(NSError **)error;
- (NSData *)remove:(NSString *)package status:(NSData *)status error:(NSError **)error;
- (BOOL)performFinish:(NSData *)status error:(NSError **)error;
// Caller must own a stopped machine, have explicit force-off consent, and
// finish the existing jailbreak builder/recovery before invoking this method.
+ (BOOL)prepareStoppedMachine:(NSString *)directory instanceID:(NSString *)identifier error:(NSError **)error;
// Synchronous worker callback. fraction is phase-local, or -1 if indeterminate.
+ (BOOL)prepareStoppedMachine:(NSString *)directory instanceID:(NSString *)identifier
                    progress:(VMPackageSetupProgress)progress error:(NSError **)error;
@end
