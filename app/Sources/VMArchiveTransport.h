// HTTPS-only recovery for the public Saurik archive. No trust exceptions.
#import <Foundation/Foundation.h>

static inline NSURL * _Nullable VMArchiveFallbackURL(NSURL * _Nullable url, NSError * _Nullable error) {
    if (![error.domain isEqual:NSURLErrorDomain]) return nil;
    switch (error.code) {
        case NSURLErrorSecureConnectionFailed:
        case NSURLErrorServerCertificateHasBadDate:
        case NSURLErrorServerCertificateUntrusted:
        case NSURLErrorServerCertificateHasUnknownRoot:
        case NSURLErrorServerCertificateNotYetValid:
            break;
        default: return nil; // In particular, never turn cancellation into retry.
    }
    if (![url.scheme.lowercaseString isEqual:@"https"] ||
        ![url.host.lowercaseString isEqual:@"apt.saurik.com"] ||
        (url.port && url.port.integerValue != 443) || url.user || url.password ||
        url.query || url.fragment ||
        (![url.path hasPrefix:@"/cydia/"] && ![url.path hasPrefix:@"/cydia-3.7/"])) return nil;
    // apt.saurik.com's published CloudFront CNAME, verified 2026-10-17. Its
    // default HTTPS certificate remains valid when the custom-domain cert
    // expires. Never use this for other repositories, credentials or firmware.
    NSURLComponents *parts = [NSURLComponents componentsWithURL:url resolvingAgainstBaseURL:YES];
    parts.host = @"d35rlteizluyd2.cloudfront.net";
    parts.port = nil;
    return parts.URL;
}
