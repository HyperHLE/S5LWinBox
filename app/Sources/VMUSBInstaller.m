// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMUSBInstaller.h"
#import "VMUSBService.h"
#import "VMUSBLockdown.h"
#import "VMUSBAFC.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static NSError *InstallError(NSString *message) {
    return [NSError errorWithDomain:@"S5LBox.GuestInstaller" code:1 userInfo:@{NSLocalizedDescriptionKey:message}];
}
@implementation VMUSBInstaller {
    VMUSBTransport *_transport;
    NSString *_identifier;
}
- (instancetype)initWithTransport:(VMUSBTransport *)transport instanceID:(NSString *)identifier {
    self = [super init]; if (self) { _transport = transport; _identifier = [identifier copy]; } return self;
}
- (void)report:(NSString *)message fraction:(double)fraction {
    if (self.progress) self.progress(message, fraction);
}
- (BOOL)installURL:(NSURL *)url error:(NSError **)error {
    if (NSThread.isMainThread) { if (error) *error = InstallError(@"Installation requires a worker thread."); return NO; }
    int fd = url.isFileURL ? open(url.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW | O_CLOEXEC) : -1;
    struct stat initial;
    if (fd < 0 || fstat(fd, &initial) || !S_ISREG(initial.st_mode) || initial.st_size < 1 || initial.st_size > 512ll * 1024 * 1024) {
        if (fd >= 0) close(fd);
        if (error) *error = InstallError(@"Choose a regular IPA file no larger than 512 MB."); return NO;
    }
    BOOL complete = NO, submitted = NO, terminal = NO, staged = NO;
    VMUSBLockdown *lockdown = nil;
    VMUSBService *files = nil, *installer = nil;
    VMUSBAFC *afc = nil;
    NSString *path = [NSString stringWithFormat:@"PublicStaging/S5LBox-%@.ipa", NSUUID.UUID.UUIDString];
    NSError *failure = nil;
    __weak VMUSBInstaller *weakSelf = self;
    do {
        if (self.canceled) { failure = InstallError(@"Installation canceled."); break; }
        [self report:@"Connecting to the guest’s virtual USB…" fraction:-1];
        lockdown = [VMUSBLockdown openWithTransport:_transport instanceID:_identifier
            canceled:^BOOL{ return !weakSelf || weakSelf.canceled; } deadline:0 error:&failure];
        if (!lockdown) break;
        [self report:@"Opening the guest’s installation services…" fraction:-1];
        files = [lockdown openService:@"com.apple.afc" error:&failure];
        if (!files) break;
        installer = [lockdown openService:@"com.apple.mobile.installation_proxy" error:&failure];
        if (!installer) break;
        // Disconnecting this session leaves the independently opened services
        // alive and releases a mux slot. No host-side guest filesystem access.
        [lockdown close]; lockdown = nil;
        afc = [[VMUSBAFC alloc] initWithService:files];
        if (![afc makeStagingDirectory:&failure]) break;
        staged = YES; // open may reach the guest even if its reply is lost
        uint64_t handle = [afc openPath:path error:&failure];
        if (!handle) break;
        uint8_t buffer[65536]; off_t sent = 0;
        NSTimeInterval uploadDeadline = NSProcessInfo.processInfo.systemUptime + 900;
        while (sent < initial.st_size) {
            if (NSProcessInfo.processInfo.systemUptime >= uploadDeadline) {
                failure = InstallError(@"The IPA upload did not finish within fifteen minutes."); break;
            }
            ssize_t n = read(fd, buffer, (size_t)MIN((off_t)sizeof buffer, initial.st_size - sent));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { failure = InstallError(@"The IPA changed or could not be read during upload."); break; }
            if (![afc writeHandle:handle data:[NSData dataWithBytes:buffer length:(NSUInteger)n] error:&failure]) break;
            sent += n;
            [self report:@"Sending IPA to the guest…" fraction:0.75 * (double)sent / (double)initial.st_size];
        }
        if (failure || ![afc closeHandle:handle error:&failure]) break;
        struct stat final;
        if (fstat(fd, &final) || final.st_size != initial.st_size ||
            final.st_mtimespec.tv_sec != initial.st_mtimespec.tv_sec || final.st_mtimespec.tv_nsec != initial.st_mtimespec.tv_nsec) {
            failure = InstallError(@"The IPA changed during upload. Try again with an unchanged file."); break;
        }
        [self report:@"The guest is installing the app…" fraction:0.75];
        submitted = YES; // even a failed write can have reached the installer
        if (![installer sendPlist:@{@"Command":@"Install", @"PackagePath":path,
                                   @"ClientOptions":@{@"ApplicationType":@"User"}} error:&failure]) break;
        NSTimeInterval deadline = NSProcessInfo.processInfo.systemUptime + 600;
        while (NSProcessInfo.processInfo.systemUptime < deadline) {
            NSDictionary *reply = [installer receivePlist:&failure];
            if (!reply) break;
            if (reply[@"Error"]) {
                terminal = YES;
                NSString *guidance = [reply[@"Error"] isEqual:@"ApplicationVerificationFailed"]
                    ? @"The guest did not accept its signature. Unsigned homebrew needs compatible AppSync installed inside the guest; on iPhone OS 3.1.3, use AppSync for OS 3.1."
                    : @"Its signing and iPhone OS compatibility must be accepted by the guest.";
                failure = InstallError([NSString stringWithFormat:@"The guest rejected this IPA: %@. %@",
                    reply[@"Error"], reply[@"ErrorDescription"] ?: guidance]);
                break;
            }
            if ([reply[@"Status"] isEqual:@"Complete"]) { complete = terminal = YES; break; }
            NSNumber *percent = reply[@"PercentComplete"];
            double fraction = [percent isKindOfClass:NSNumber.class] ? 0.75 + 0.25 * MAX(0, MIN(100, percent.doubleValue)) / 100.0 : -1;
            NSString *status = [reply[@"Status"] isKindOfClass:NSString.class] ? reply[@"Status"] : @"Installing";
            [self report:[NSString stringWithFormat:@"Guest: %@…", status] fraction:fraction];
        }
        if (!complete && !failure) failure = InstallError(@"The guest installer did not finish within ten minutes.");
    } while (0);
    close(fd);
    // Only this attempt's unique file, and never while installation may still
    // be running. An interrupted request is explicitly an unknown outcome.
    if (staged && (!submitted || terminal)) {
        // Best-effort cleanup must not turn Cancel into another two-minute
        // service wait, especially after a partial/unsynchronized AFC reply.
        NSTimeInterval cleanupDeadline = NSProcessInfo.processInfo.systemUptime + 2.0;
        files.canceled = ^BOOL{ return NSProcessInfo.processInfo.systemUptime >= cleanupDeadline; };
        [afc removePath:path error:NULL];
    }
    [lockdown close]; [files close]; [installer close];
    if (!complete && submitted && !terminal)
        failure = InstallError([NSString stringWithFormat:@"%@ The guest may still finish installing. Check its Home screen before retrying.",
                                failure.localizedDescription ?: @"The connection ended."]);
    if (!complete && error) *error = failure ?: InstallError(@"Installation did not finish.");
    if (complete) [self report:@"Installed by the guest. Check its Home screen to open the app." fraction:1];
    return complete;
}
@end
