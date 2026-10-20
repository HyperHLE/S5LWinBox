// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMGuestFiles.h"
#import "VMUSBAFC.h"
#import "VMUSBLockdown.h"
#import "VMUSBService.h"
#include "VMGuestFilesService.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

@implementation VMGuestFile
@end
static NSError *FilesError(NSString *message) {
    return [NSError errorWithDomain:@"S5LBox.Files" code:1 userInfo:@{NSLocalizedDescriptionKey:message}];
}
static BOOL FileSize(NSString *text, uint64_t *size) {
    if (![text isKindOfClass:NSString.class] || !text.length || text.length > 20) return NO;
    uint64_t result = 0;
    for (NSUInteger i = 0; i < text.length; i++) {
        unichar c = [text characterAtIndex:i];
        if (c < '0' || c > '9' || result > (UINT64_MAX - (c - '0')) / 10) return NO;
        result = result * 10 + c - '0';
    }
    *size = result; return YES;
}
@implementation VMGuestFiles {
    VMUSBTransport *_transport;
    NSString *_identifier;
    BOOL _root;
}
- (instancetype)initWithTransport:(VMUSBTransport *)transport instanceID:(NSString *)identifier root:(BOOL)root {
    self = [super init]; if (self) { _transport = transport; _identifier = [identifier copy]; _root = root; } return self;
}
- (VMUSBService *)connect:(NSError **)error {
    if (NSThread.isMainThread || self.canceled) { if (error) *error = FilesError(@"File operation canceled."); return nil; }
    __weak VMGuestFiles *weakSelf = self;
    BOOL (^cancel)(void) = ^BOOL{ return !weakSelf || weakSelf.canceled; };
    // Stock lockdownd can generate/pair keys during a cold boot. Retain the
    // installer's 120-second connect allowance; the unpaired root endpoint
    // needs no cryptography and can fail sooner. Both remain cancelable.
    NSTimeInterval deadline = NSProcessInfo.processInfo.systemUptime + (_root ? 20 : 120);
    if (_root) {
        VMUSBService *service = [[VMUSBService alloc] initWithTransport:_transport port:VM_GUEST_FILES_PORT
            canceled:cancel deadline:deadline error:error];
        if (!service && error && !self.canceled) *error = FilesError(@"Full filesystem access is unavailable. Start the guest fully and retry. For an older jailbreak, shut down the machine and run Jailbreak again to add Files support.");
        [service setOperationDeadline:NSProcessInfo.processInfo.systemUptime + 900];
        return service;
    }
    VMUSBLockdown *lockdown = [VMUSBLockdown openWithTransport:_transport instanceID:_identifier
        canceled:cancel deadline:deadline error:error];
    VMUSBService *service = [lockdown openService:@"com.apple.afc" error:error]; [lockdown close];
    [service setOperationDeadline:NSProcessInfo.processInfo.systemUptime + 900]; return service;
}
- (id)withFiles:(id (^)(VMUSBAFC *, NSError **))operation error:(NSError **)error {
    VMUSBService *service = [self connect:error]; if (!service) return nil;
    id result = operation([[VMUSBAFC alloc] initForFileBrowserWithService:service], error);
    [service close]; return result;
}
- (NSArray<VMGuestFile *> *)list:(NSString *)path error:(NSError **)error {
    return [self withFiles:^id(VMUSBAFC *afc, NSError **failure) {
        NSArray *names = [afc contentsOfDirectory:path error:failure]; if (!names) return nil;
        NSMutableArray *files = [NSMutableArray array];
        for (NSString *name in names) {
            NSString *child = [path stringByAppendingPathComponent:name];
            NSDictionary *info = [afc attributesOfPath:child error:failure]; if (!info) return nil;
            VMGuestFile *file = [VMGuestFile new]; file.name = name; file.path = child; file.kind = info[@"st_ifmt"];
            uint64_t size = 0;
            if ([file.kind isEqual:@"S_IFREG"] && !FileSize(info[@"st_size"], &size)) {
                if (failure) *failure = FilesError(@"The guest returned an invalid file size."); return nil;
            }
            file.size = size; [files addObject:file];
        }
        [files sortUsingComparator:^NSComparisonResult(VMGuestFile *a, VMGuestFile *b) {
            BOOL ad = [a.kind isEqual:@"S_IFDIR"], bd = [b.kind isEqual:@"S_IFDIR"];
            return ad != bd ? (ad ? NSOrderedAscending : NSOrderedDescending) : [a.name localizedStandardCompare:b.name];
        }];
        return files;
    } error:error];
}
- (BOOL)absent:(NSString *)path afc:(VMUSBAFC *)afc error:(NSError **)error {
    NSError *failure = nil;
    if ([afc attributesOfPath:path error:&failure]) { if (error) *error = FilesError(@"An item with this name already exists. Choose another name."); return NO; }
    if ([failure.domain isEqual:@"S5LBox.AFC"] && failure.code == 8) return YES;
    if (error) *error = failure ?: FilesError(@"Could not check the destination."); return NO;
}
- (NSURL *)download:(VMGuestFile *)file intoDirectory:(NSURL *)directory error:(NSError **)error {
    if (![file.kind isEqual:@"S_IFREG"] || file.size > 512ull * 1024 * 1024 || ![VMUSBAFC validFilename:file.name]) {
        if (error) *error = FilesError(@"Only regular files up to 512 MB can be exported."); return nil;
    }
    NSURL *target = [directory URLByAppendingPathComponent:file.name];
    return [self withFiles:^id(VMUSBAFC *afc, NSError **failure) {
        NSDictionary *info = [afc attributesOfPath:file.path error:failure]; uint64_t size;
        if (![info[@"st_ifmt"] isEqual:@"S_IFREG"] || !FileSize(info[@"st_size"], &size) || size != file.size) {
            if (failure && !*failure) *failure = FilesError(@"The guest file changed. Refresh and try again."); return nil;
        }
        int fd = open(target.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) { if (failure) *failure = FilesError(@"Could not create the local export."); return nil; }
        uint64_t handle = [afc openBrowserPath:file.path writing:NO error:failure], done = 0; BOOL okay = handle != 0;
        while (okay && done < size) {
            NSData *part = [afc readHandle:handle count:(NSUInteger)MIN(65536ull, size - done) error:failure];
            if (!part.length) { okay = NO; break; }
            NSUInteger offset = 0;
            while (offset < part.length) {
                ssize_t n = write(fd, (const uint8_t *)part.bytes + offset, part.length - offset);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) { okay = NO; break; } offset += (NSUInteger)n;
            }
            done += part.length; if (self.progress) self.progress(size ? (double)done / size : 1);
        }
        if (handle && ![afc closeHandle:handle error:failure]) okay = NO;
        if (close(fd)) okay = NO;
        if (!okay) { unlink(target.fileSystemRepresentation); if (failure && !*failure) *failure = FilesError(@"The file could not be exported completely."); return nil; }
        return target;
    } error:error];
}
- (BOOL)upload:(NSURL *)source directory:(NSString *)directory error:(NSError **)error {
    if (![VMUSBAFC validFilename:source.lastPathComponent]) { if (error) *error = FilesError(@"Choose a file with a valid name."); return NO; }
    BOOL scope = [source startAccessingSecurityScopedResource];
    __block BOOL success = NO; __block NSError *failure = nil;
    NSError *coordinationError = nil;
    NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    [coordinator coordinateReadingItemAtURL:source options:0 error:&coordinationError byAccessor:^(NSURL *url) {
        int fd = url.isFileURL ? open(url.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW | O_CLOEXEC) : -1;
        struct stat initial;
        if (fd < 0 || fstat(fd, &initial) || !S_ISREG(initial.st_mode) || initial.st_size < 0 || initial.st_size > 512ll * 1024 * 1024) {
            if (fd >= 0) close(fd); failure = FilesError(@"Choose a regular file no larger than 512 MB."); return;
        }
        success = [[self withFiles:^id(VMUSBAFC *afc, NSError **problem) {
            NSString *final = [directory stringByAppendingPathComponent:source.lastPathComponent];
            if (![self absent:final afc:afc error:problem]) return @NO;
            NSString *temp = [directory stringByAppendingPathComponent:[@".s5lbox-import-" stringByAppendingString:NSUUID.UUID.UUIDString]];
            if (![self absent:temp afc:afc error:problem]) return @NO;
            uint64_t handle = [afc openBrowserPath:temp writing:YES error:problem]; BOOL okay = handle != 0;
            uint8_t buffer[65536]; off_t done = 0;
            while (okay && done < initial.st_size) {
                ssize_t n = read(fd, buffer, (size_t)MIN((off_t)sizeof buffer, initial.st_size - done));
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0 || ![afc writeHandle:handle data:[NSData dataWithBytes:buffer length:(NSUInteger)MAX(n, 0)] error:problem]) { okay = NO; break; }
                done += n; if (self.progress) self.progress((double)done / initial.st_size);
            }
            if (handle && ![afc closeHandle:handle error:problem]) okay = NO;
            struct stat after;
            if (fstat(fd, &after) || after.st_size != initial.st_size ||
                after.st_mtimespec.tv_sec != initial.st_mtimespec.tv_sec || after.st_mtimespec.tv_nsec != initial.st_mtimespec.tv_nsec) okay = NO;
            if (okay) okay = [self absent:final afc:afc error:problem] && [afc renameBrowserPath:temp toPath:final error:problem];
            if (!okay) {
                [afc removeBrowserPath:temp error:NULL];
                if (problem && !*problem) *problem = FilesError(@"The import did not finish. A .s5lbox-import file may remain in the guest if it disconnected.");
            }
            return @(okay);
        } error:&failure] boolValue];
        close(fd);
    }];
    if (scope) [source stopAccessingSecurityScopedResource];
    if (!success && error) *error = failure ?: coordinationError ?: FilesError(@"The file could not be imported."); return success;
}
- (BOOL)createDirectory:(NSString *)path error:(NSError **)error {
    return [[self withFiles:^id(VMUSBAFC *afc, NSError **failure) {
        return @([self absent:path afc:afc error:failure] && [afc createDirectory:path error:failure]);
    } error:error] boolValue];
}
- (BOOL)rename:(VMGuestFile *)file name:(NSString *)name error:(NSError **)error {
    if (![VMUSBAFC validFilename:name]) { if (error) *error = FilesError(@"Enter a filename without slashes or control characters."); return NO; }
    NSString *destination = [file.path.stringByDeletingLastPathComponent stringByAppendingPathComponent:name];
    return [[self withFiles:^id(VMUSBAFC *afc, NSError **failure) {
        return @([self absent:destination afc:afc error:failure] && [afc renameBrowserPath:file.path toPath:destination error:failure]);
    } error:error] boolValue];
}
- (BOOL)remove:(VMGuestFile *)file error:(NSError **)error {
    return [[self withFiles:^id(VMUSBAFC *afc, NSError **failure) {
        // AFC_REMOVE_PATH is nonrecursive; never request REMOVE_PATH_AND_CONTENTS.
        return @([afc removeBrowserPath:file.path error:failure]);
    } error:error] boolValue];
}
@end
