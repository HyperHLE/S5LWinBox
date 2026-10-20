// Synthetic transport tests: no public downloads or guest execution.
#import "VMGuestPackageDownloader.h"
#import "VMArchiveTransport.h"
#import "VMGuestPackageManifest.h"
#include <assert.h>

@interface VMGuestPackageDownloader (TransportTests) <NSURLSessionDownloadDelegate>
- (void)startDownloadAtURL:(NSURL *)url filename:(NSString *)filename;
- (void)finishWithDirectory:(NSURL *)directory error:(NSError *)error;
@end

@interface DownloadTaskFixture : NSObject
@property(nonatomic, strong) NSURLRequest *originalRequest;
@property(nonatomic, strong) NSURLResponse *response;
@end
@implementation DownloadTaskFixture
@end

@interface PackageDownloadFixture : VMGuestPackageDownloader
@property NSUInteger starts;
@property NSUInteger finishes;
@property(nonatomic, strong) NSURL *requested;
@property(nonatomic, strong) NSError *failure;
@end
@implementation PackageDownloadFixture
- (void)startDownloadAtURL:(NSURL *)url filename:(NSString *)filename {
    assert(filename.length); self.starts++; self.requested = url;
}
- (void)finishWithDirectory:(NSURL *)directory error:(NSError *)error {
    (void)directory; self.finishes++; self.failure = error;
}
@end

int main(int argc, const char *argv[]) { @autoreleasepool {
    assert(argc == 2);
    NSURL *root = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]] isDirectory:YES];
    NSURL *url = [NSURL URLWithString:@"https://apt.saurik.com/cydia-3.7/debs/apt7-lib_0.7.20.2-1_iphoneos-arm.deb"];
    NSError *tls = [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorServerCertificateHasBadDate userInfo:nil];
    NSError *cancel = [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorCancelled userInfo:nil];
    NSURL *cdn = VMArchiveFallbackURL(url,tls);
    assert([cdn.host isEqual:@"d35rlteizluyd2.cloudfront.net"]);
    assert([cdn.scheme isEqual:@"https"] && [cdn.path isEqual:url.path]);
    assert(!VMArchiveFallbackURL(cdn,tls)); // One alternate, never a retry loop.
    assert(!VMArchiveFallbackURL(url,cancel));
    assert(!VMArchiveFallbackURL(url,nil));
    assert(!VMArchiveFallbackURL(url,[NSError errorWithDomain:@"other" code:tls.code userInfo:nil]));
    for (NSString *bad in @[@"http://apt.saurik.com/cydia/Packages",
        @"https://apt.saurik.com.evil.example/cydia/Packages",
        @"https://user:secret@apt.saurik.com/cydia/Packages",
        @"https://apt.saurik.com:8443/cydia/Packages",
        @"https://apt.saurik.com/cydia/Packages?token=secret",
        @"https://apt.saurik.com/cydia/Packages#fragment",
        @"https://apt.saurik.com/other/file"])
        assert(!VMArchiveFallbackURL([NSURL URLWithString:bad],tls));
    assert(VMArchiveFallbackURL([NSURL URLWithString:@"https://apt.saurik.com/cydia/Packages.bz2"],tls));

    DownloadTaskFixture *task = [DownloadTaskFixture new];
    NSURLSession *session = NSURLSession.sharedSession; // No tasks are started.
    task.originalRequest = [NSURLRequest requestWithURL:url];
    PackageDownloadFixture *d = [PackageDownloadFixture new];
    [d setValue:@2 forKey:@"index"];
    [d URLSession:session task:(NSURLSessionTask *)task didCompleteWithError:tls];
    assert(d.starts == 1 && !d.finishes && [d.requested isEqual:cdn]);
    task.originalRequest = [NSURLRequest requestWithURL:cdn];
    [d URLSession:session task:(NSURLSessionTask *)task didCompleteWithError:tls];
    assert(d.starts == 1 && d.finishes == 1 && d.failure == tls);

    d = [PackageDownloadFixture new];
    [d setValue:@YES forKey:@"cancelled"];
    task.originalRequest = [NSURLRequest requestWithURL:url];
    [d URLSession:session task:(NSURLSessionTask *)task didCompleteWithError:tls];
    assert(!d.starts && d.finishes == 1 && [d.failure.localizedDescription containsString:@"cancelled"]);

    // The alternate path still refuses HTTP and requires the exact pinned
    // archive identity before publication. Corrupt bytes cannot trigger retry.
    d = [PackageDownloadFixture new];
    [d setValue:@2 forKey:@"index"];
    __block NSURLRequest *redirect = task.originalRequest;
    NSURLRequest *insecure = [NSURLRequest requestWithURL:[NSURL URLWithString:@"http://apt.saurik.com/cydia/Packages"]];
    NSHTTPURLResponse *redirectResponse = [[NSHTTPURLResponse alloc] initWithURL:url statusCode:302 HTTPVersion:@"HTTP/1.1" headerFields:nil];
    [d URLSession:session task:(NSURLSessionTask *)task willPerformHTTPRedirection:redirectResponse newRequest:insecure
        completionHandler:^(NSURLRequest *next) { redirect = next; }];
    assert(!redirect);
    NSURL *incoming = [root URLByAppendingPathComponent:@"invalid-package-input"];
    NSURL *partial = [root URLByAppendingPathComponent:@"invalid-package.partial"];
    assert(![NSFileManager.defaultManager fileExistsAtPath:incoming.path]);
    assert(![NSFileManager.defaultManager fileExistsAtPath:partial.path]);
    assert([[@"invalid" dataUsingEncoding:NSUTF8StringEncoding] writeToURL:incoming atomically:YES]);
    [d setValue:partial forKey:@"partialURL"];
    task.response = [[NSHTTPURLResponse alloc] initWithURL:cdn statusCode:200 HTTPVersion:@"HTTP/1.1" headerFields:nil];
    [d URLSession:session downloadTask:(NSURLSessionDownloadTask *)task didFinishDownloadingToURL:incoming];
    assert(![NSFileManager.defaultManager fileExistsAtPath:partial.path]);
    assert(![NSFileManager.defaultManager fileExistsAtPath:incoming.path]);
    [d URLSession:session task:(NSURLSessionTask *)task didCompleteWithError:tls];
    assert(!d.starts && d.finishes == 1 && [d.failure.localizedDescription containsString:@"was rejected"]);

    // The optional last slot uses its own HTTPS source and exact digest; an
    // unrelated Saurik CDN fallback cannot be applied to an AppSync failure.
    d = [PackageDownloadFixture new];
    [d setValue:@YES forKey:@"includeAppSync"];
    [d setValue:@(vm_guest_package_count()) forKey:@"index"];
    NSURL *appSyncURL = [NSURL URLWithString:@(vm_guest_package_appsync()->source_url)];
    task.originalRequest = [NSURLRequest requestWithURL:appSyncURL];
    assert([[@"corrupt AppSync" dataUsingEncoding:NSUTF8StringEncoding] writeToURL:incoming atomically:YES]);
    [d setValue:partial forKey:@"partialURL"];
    task.response = [[NSHTTPURLResponse alloc] initWithURL:appSyncURL statusCode:200 HTTPVersion:@"HTTP/1.1" headerFields:nil];
    [d URLSession:session downloadTask:(NSURLSessionDownloadTask *)task didFinishDownloadingToURL:incoming];
    assert(![NSFileManager.defaultManager fileExistsAtPath:incoming.path]);
    assert(![NSFileManager.defaultManager fileExistsAtPath:partial.path]);
    [d URLSession:session task:(NSURLSessionTask *)task didCompleteWithError:tls];
    assert(!d.starts && d.finishes == 1 && [d.failure.localizedDescription containsString:@"us.hackulo.appsync31"]);
    puts("Guest package transport: HTTPS alternate, bounded retry, cancellation and identity rejection passed");
    return 0;
} }
