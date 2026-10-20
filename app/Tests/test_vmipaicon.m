// Synthetic ZIP and PNG fixtures only. No guest or user files are touched.
#import "VMIPAIcon.h"
#include "VMFirmwareFixtures.h"
#include <assert.h>

static NSData *Archive(id info, NSDictionary<NSString *, NSData *> *icons) {
    NSData *plist = [NSPropertyListSerialization dataWithPropertyList:info
        format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
    assert(plist);
    fx_zip_member_t members[12] = {{"Payload/Test.app/Info.plist", plist.bytes, plist.length, false, false}};
    NSArray<NSString *> *names = [icons.allKeys sortedArrayUsingSelector:@selector(compare:)];
    assert(names.count < 12);
    for (NSUInteger i = 0; i < names.count; i++) {
        NSData *data = icons[names[i]];
        members[i + 1] = (fx_zip_member_t){names[i].UTF8String, data.bytes, data.length, false, false};
    }
    NSUInteger capacity = plist.length + 4096;
    for (NSData *data in icons.allValues) capacity += data.length;
    NSMutableData *zip = [NSMutableData dataWithLength:capacity];
    size_t size = fx_zip_build(zip.mutableBytes, zip.length, members, (unsigned)names.count + 1, NULL);
    assert(size); zip.length = size;
    return zip;
}
static NSArray<NSData *> *Inspect(NSURL *file, id info, NSDictionary *icons) {
    assert([Archive(info, icons) writeToURL:file options:NSDataWritingAtomic error:NULL]);
    return [VMIPAIcon imageDataCandidatesForURL:file];
}
int main(int argc, const char *argv[]) {
    @autoreleasepool {
        assert(argc == 2);
        NSURL *root = [[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]] isDirectory:YES]
            URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES];
        assert([NSFileManager.defaultManager createDirectoryAtURL:root withIntermediateDirectories:YES attributes:nil error:NULL]);
        NSURL *file = [root URLByAppendingPathComponent:@"Test.ipa"];
        NSData *png = [[NSData alloc] initWithBase64EncodedString:
            @"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/l9sAAAAASUVORK5CYII=" options:0];
        NSString *legacy = @"Payload/Test.app/icon.png";
        assert([Inspect(file, @{}, @{legacy:png}).firstObject isEqual:png]);
        // Both metadata and art may be safe in-bundle links. The archive itself
        // is still a regular host file; no host symlink is ever followed.
        NSData *plist = [NSPropertyListSerialization dataWithPropertyList:@{}
            format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
        fx_zip_member_t linked[] = {
            {"Payload/Test.app/Info.plist", (const uint8_t *)"Metadata", 8, false, false},
            {"Payload/Test.app/Metadata", plist.bytes, plist.length, false, false},
            {"Payload/Test.app/Icon.png", (const uint8_t *)"Art.png", 7, true, true},
            {"Payload/Test.app/Art.png", png.bytes, png.length, false, false}
        };
        NSMutableData *linkedZIP = [NSMutableData dataWithLength:4096];
        fx_zip_layout_t linkedLayout;
        size_t linkedSize = fx_zip_build(linkedZIP.mutableBytes, linkedZIP.length, linked, 4, &linkedLayout);
        assert(linkedSize); linkedZIP.length = linkedSize;
        for (unsigned i = 0; i < 4; i += 2) {
            uint8_t *central = (uint8_t *)linkedZIP.mutableBytes + linkedLayout.central_offset[i];
            fx_w16le(central + 4, (3u << 8) | 20u); fx_w32le(central + 38, 0120777u << 16);
        }
        assert([linkedZIP writeToURL:file options:NSDataWritingAtomic error:NULL]);
        assert([[VMIPAIcon imageDataCandidatesForURL:file].firstObject isEqual:png]);
        // A real legacy pattern: the plist points at a missing Resources icon.
        assert([Inspect(file, @{@"CFBundleIconFile":@"Resources/icon.png"}, @{legacy:png}).firstObject isEqual:png]);
        NSString *custom = @"Payload/Test.app/Art/Launcher.png";
        assert([Inspect(file, @{@"CFBundleIconFile":@"Art/Launcher"}, @{custom:png}).firstObject isEqual:png]);
        assert(Inspect(file, @{@"CFBundleIconFile":@"../outside"}, @{custom:png}).count == 0);
        assert([Inspect(file, @{@"CFBundleIconFile":@17, @"CFBundleIconFiles":@[@{}, @9]}, @{legacy:png}).firstObject isEqual:png]);
        assert([Inspect(file, @{@"CFBundleIcons":@{@"CFBundlePrimaryIcon":@{@"CFBundleIconFiles":@[@"Art/Launcher"]}}}, @{custom:png}).firstObject isEqual:png]);
        assert([Inspect(file, @{@"CFBundleIconFiles":@[@"Launch"]}, @{@"Payload/Test.app/Launch@2x.png":png}).firstObject isEqual:png]);
        assert(Inspect(file, @[], @{legacy:png}).count == 0);
        assert(Inspect(file, @{}, @{}).count == 0);
        assert(Inspect(file, @{}, @{legacy:[@"not PNG" dataUsingEncoding:NSUTF8StringEncoding]}).count == 0);
        NSMutableData *huge = [png mutableCopy];
        fx_w32be((uint8_t *)huge.mutableBytes + 16, 100000);
        assert(Inspect(file, @{}, @{legacy:huge}).count == 0);
        NSMutableData *optimized = [NSMutableData dataWithBytes:png.bytes length:8];
        uint8_t cgbi[16] = {0,0,0,4,'C','g','B','I',0,0,0,0,0,0,0,0};
        [optimized appendBytes:cgbi length:sizeof cgbi];
        [optimized appendBytes:(const uint8_t *)png.bytes + 8 length:png.length - 8];
        assert([Inspect(file, @{}, @{legacy:optimized}).firstObject isEqual:optimized]);
        assert([Inspect(file, @{@"CFBundleIconFiles":@[@"Launch"]},
            @{@"Payload/Test.app/Launch.png":png, @"Payload/Test.app/Launch@2x.png":optimized}).firstObject isEqual:optimized]);
        assert(Inspect(file, @{@"CFBundleIconFile":@"icon.png"}, @{legacy:png}).count == 1);
        assert(Inspect(file, @{}, @{@"Payload/Test.app/../icon.png":png}).count == 0);
        NSMutableData *oversized = [png mutableCopy]; oversized.length = 2u * 1024u * 1024u + 1;
        assert(Inspect(file, @{}, @{legacy:oversized}).count == 0);
        NSString *largeValue = [@"x" stringByPaddingToLength:1024u * 1024u withString:@"x" startingAtIndex:0];
        assert(Inspect(file, @{@"Extra":largeValue}, @{legacy:png}).count == 0);
        NSMutableData *badCRC = [Archive(@{}, @{legacy:png}) mutableCopy];
        NSRange pixels = [badCRC rangeOfData:png options:0 range:NSMakeRange(0, badCRC.length)];
        assert(pixels.location != NSNotFound);
        ((uint8_t *)badCRC.mutableBytes)[NSMaxRange(pixels) - 1] ^= 1;
        assert([badCRC writeToURL:file options:NSDataWritingAtomic error:NULL]);
        assert([VMIPAIcon imageDataCandidatesForURL:file].count == 0);
        // The same path is replaced repeatedly: cached art must follow the file,
        // not its name, and a failed icon may not become permanently cached.
        assert([Inspect(file, @{}, @{legacy:png}).firstObject isEqual:png]);
        assert([[VMIPAIcon imageDataCandidatesForURL:file].firstObject isEqual:png]);
        assert(Inspect(file, @{}, @{}).count == 0);
        assert([Inspect(file, @{}, @{legacy:png}).firstObject isEqual:png]);
        NSURL *link = [root URLByAppendingPathComponent:@"Link.ipa"];
        assert([NSFileManager.defaultManager createSymbolicLinkAtURL:link withDestinationURL:file error:NULL]);
        assert([VMIPAIcon imageDataCandidatesForURL:link].count == 0);
        assert([VMIPAIcon imageDataCandidatesForURL:[NSURL URLWithString:@"https://example.invalid/app.ipa"]].count == 0);
        assert([NSFileManager.defaultManager removeItemAtURL:root error:NULL]);
        puts("IPA icons: metadata, scale variants, legacy fallback, CgBI header, bounds, CRC, malformed input, cache replacement and symlink refusal passed");
    }
    return 0;
}
