// Credential publication state, using the actual bridge implementation.
// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import "../Sources/VMPackageBridge.m"

static NSURL *testDirectory;
@interface NSFileManager (PackageTestPaths)
- (NSArray<NSURL *> *)packageTestURLs:(NSSearchPathDirectory)directory inDomains:(NSSearchPathDomainMask)domain;
@end
@implementation NSFileManager (PackageTestPaths)
- (NSArray<NSURL *> *)packageTestURLs:(NSSearchPathDirectory)directory inDomains:(NSSearchPathDomainMask)domain {
    if (directory == NSApplicationSupportDirectory) return @[testDirectory];
    return [self packageTestURLs:directory inDomains:domain];
}
@end

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr,"line %d: %s\n",__LINE__,#x); } } while (0)

// Exercise status framing without a device, sockets or a mutable guest disk.
@interface PackageStatusFixture : VMUSBService
@property(nonatomic, strong) NSMutableData *bytes;
@end
@implementation PackageStatusFixture
- (NSData *)readCount:(NSUInteger)count error:(NSError **)error {
    if (count > self.bytes.length) {
        if (error) *error=VMPackageError(@"Short fixture response.");
        return nil;
    }
    NSData *part=[self.bytes subdataWithRange:NSMakeRange(0,count)];
    [self.bytes replaceBytesInRange:NSMakeRange(0,count) withBytes:NULL length:0];
    return part;
}
- (void)close {}
@end
@interface PackageStatusBridge : VMPackageBridge
@property(nonatomic, strong) PackageStatusFixture *fixture;
@end
@implementation PackageStatusBridge
- (VMUSBService *)connect:(char)operation error:(NSError **)error {
    (void)error; CHECK(operation=='S'); return self.fixture;
}
@end
static void StatusFrame(PackageStatusFixture *fixture, char kind, NSData *data) {
    uint32_t size=htonl((uint32_t)data.length+1);
    [fixture.bytes appendBytes:&size length:4];
    [fixture.bytes appendBytes:&kind length:1];
    [fixture.bytes appendData:data];
}
static void TestFirstBootReadiness(void) {
    PackageStatusBridge *bridge=[PackageStatusBridge new];
    bridge.fixture=[PackageStatusFixture new];
    bridge.fixture.bytes=[NSMutableData data];
    NSString *pending=@"Jailbreak setup is still finishing. Wait for it to complete, then refresh Packages and retry.";
    StatusFrame(bridge.fixture,'E',[pending dataUsingEncoding:NSUTF8StringEncoding]);
    NSError *error=nil;
    CHECK([bridge status:&error]==nil && bridge.setupPending);
    CHECK([error.localizedDescription isEqual:pending]);
    CHECK(!bridge.finishReady && bridge.finishAction==0);

    NSData *status=[@"Package: fixture\nStatus: install ok installed\n\n" dataUsingEncoding:NSUTF8StringEncoding];
    const unsigned char finish[2]={0,1};
    StatusFrame(bridge.fixture,'F',[NSData dataWithBytes:finish length:2]);
    StatusFrame(bridge.fixture,'S',status);
    error=nil;
    CHECK([[bridge status:&error] isEqual:status] && error==nil);
    CHECK(!bridge.setupPending && bridge.finishReady);

    StatusFrame(bridge.fixture,'E',[@"Database unavailable." dataUsingEncoding:NSUTF8StringEncoding]);
    CHECK([bridge status:&error]==nil && !bridge.setupPending);
    CHECK([error.localizedDescription containsString:@"Cannot connect to iPhone OS."]);
}
int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc != 2) return 2;
        TestFirstBootReadiness();
        testDirectory = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]] isDirectory:YES];
        method_exchangeImplementations(class_getInstanceMethod(NSFileManager.class,@selector(URLsForDirectory:inDomains:)),
            class_getInstanceMethod(NSFileManager.class,@selector(packageTestURLs:inDomains:)));
        NSString *identifier = @"0123456789abcdef";
        CHECK(![VMPackageBridge hasCapabilityForInstance:identifier]);
        CHECK(PackageSetupPending(identifier,YES,NULL));
        NSData *token = Capability(identifier,YES,NULL);
        CHECK(token.length == 32);
        CHECK(![VMPackageBridge hasCapabilityForInstance:identifier]);
        // Retry after a failed or interrupted publication keeps the same token.
        CHECK(PackageSetupPending(identifier,YES,NULL));
        CHECK([Capability(identifier,YES,NULL) isEqualToData:token]);
        CHECK(![VMPackageBridge hasCapabilityForInstance:identifier]);
        CHECK(PackageSetupPending(identifier,NO,NULL));
        CHECK([VMPackageBridge hasCapabilityForInstance:identifier]);
        CHECK(PackageSetupPending(identifier,NO,NULL));
        // Existing installations without a marker remain compatible. An
        // attempted repair becomes pending until it is verified again.
        CHECK(PackageSetupPending(identifier,YES,NULL));
        CHECK(![VMPackageBridge hasCapabilityForInstance:identifier]);
        CHECK([Capability(identifier,NO,NULL) isEqualToData:token]);
        CHECK(PackageSetupPending(identifier,NO,NULL));
        CHECK([VMPackageBridge hasCapabilityForInstance:identifier]);
        // Disk publication readiness and first live connection are distinct.
        // A manually opened workspace must keep the first-start allowance even
        // after its controller was dismissed or the host app was relaunched.
        [VMPackageBridge noteSuccessfulConnectionForInstance:identifier];
        CHECK(![VMPackageBridge awaitingFirstConnectionForInstance:identifier]);
        AwaitFirstConnection(identifier);
        CHECK([VMPackageBridge awaitingFirstConnectionForInstance:identifier]);
        CHECK([NSUserDefaults.standardUserDefaults boolForKey:FirstConnectionKey(identifier)]);
        CHECK([VMPackageBridge hasCapabilityForInstance:identifier]);
        CHECK(![VMPackageBridge awaitingFirstConnectionForInstance:@"fedcba9876543210"]);
        CHECK(PackageSetupPending(identifier,YES,NULL));
        CHECK([VMPackageBridge awaitingFirstConnectionForInstance:identifier]);
        [VMPackageBridge noteSuccessfulConnectionForInstance:identifier];
        CHECK(![VMPackageBridge awaitingFirstConnectionForInstance:identifier]);
        CHECK(![VMPackageBridge hasCapabilityForInstance:identifier]);
        CHECK(PackageSetupPending(identifier,NO,NULL));
        CHECK([VMPackageBridge hasCapabilityForInstance:identifier]);
        CHECK(![VMPackageBridge awaitingFirstConnectionForInstance:identifier]);
        // Invalid identities never write a hint or affect another machine.
        AwaitFirstConnection(@"../invalid");
        CHECK(![VMPackageBridge awaitingFirstConnectionForInstance:@"../invalid"]);
        [VMPackageBridge noteSuccessfulConnectionForInstance:@"../invalid"];
        NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:CapabilityURL(identifier,NULL).path error:NULL];
        CHECK([attributes[NSFilePosixPermissions] unsignedIntValue] == 0600);
        CHECK(Capability(@"../invalid",YES,NULL) == nil);
        CHECK(!PackageSetupPending(@"../invalid",YES,NULL));
        CHECK(![VMPackageBridge hasCapabilityForInstance:@"../invalid"]);
        NSData *invalid = [@"bad" dataUsingEncoding:NSUTF8StringEncoding];
        CHECK([invalid writeToURL:CapabilityURL(identifier,NULL) atomically:YES]);
        CHECK(Capability(identifier,YES,NULL) == nil);
        CHECK(![VMPackageBridge hasCapabilityForInstance:identifier]);
        CHECK([[NSData dataWithContentsOfURL:CapabilityURL(identifier,NULL)] isEqualToData:invalid]);
        printf("Package setup credentials: %u checks, %u failures\n",checks,failures);
        return failures ? 1 : 0;
    }
}
