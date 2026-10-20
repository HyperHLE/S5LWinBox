// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMIPAIcon.h"
#import "VMIPAArchive.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static size_t IconRead(void *context, uint64_t offset, uint8_t *bytes, size_t length) {
    ssize_t count = pread(*(int *)context, bytes, length, (off_t)offset);
    return count < 0 ? 0 : (size_t)count;
}
static NSData *IconExtract(const vmfw_zip_t *zip, const vmfw_zip_entry_t *entry, NSUInteger limit) {
    if (entry->is_directory || !entry->uncompressed_size || entry->uncompressed_size > limit) return nil;
    NSMutableData *data = [NSMutableData dataWithLength:(NSUInteger)entry->uncompressed_size];
    vmfw_mem_sink_t sink;
    vmfw_mem_sink_init(&sink, data.mutableBytes, data.length);
    return vmfw_zip_extract(zip, entry, vmfw_mem_sink_write, &sink) == VMFW_ZIP_OK &&
        sink.len == data.length ? data : nil;
}
static void IconNames(NSMutableOrderedSet<NSString *> *names, id value) {
    if ([value isKindOfClass:NSArray.class]) {
        // Icon arrays commonly list the smaller image first.
        for (id item in [(NSArray *)value reverseObjectEnumerator]) {
            if ([item isKindOfClass:NSString.class]) IconNames(names, item);
            if (names.count >= 32) break;
        }
        return;
    }
    if (![value isKindOfClass:NSString.class] || ![value length] ||
        !vm_ipa_safe_path([value UTF8String]) || [value hasSuffix:@"/"] || names.count >= 32) return;
    NSString *name = value;
    if (name.pathExtension.length && [name.pathExtension caseInsensitiveCompare:@"png"] != NSOrderedSame) return;
    NSString *stem = name.pathExtension.length ? name.stringByDeletingPathExtension : name;
    if (![stem hasSuffix:@"@2x"] && ![stem hasSuffix:@"@3x"]) {
        [names addObject:[stem stringByAppendingString:@"@3x.png"]];
        [names addObject:[stem stringByAppendingString:@"@2x.png"]];
    }
    [names addObject:name.pathExtension.length ? name : [name stringByAppendingString:@".png"]];
}
static NSString *IconIdentity(NSURL *url, const struct stat *st) {
    return [NSString stringWithFormat:@"%@|%llu|%llu|%lld|%lld:%ld|%lld:%ld", url.path,
        (unsigned long long)st->st_dev, (unsigned long long)st->st_ino, (long long)st->st_size,
        (long long)st->st_mtimespec.tv_sec, st->st_mtimespec.tv_nsec,
        (long long)st->st_ctimespec.tv_sec, st->st_ctimespec.tv_nsec];
}

@implementation VMIPAIcon
+ (NSArray<NSData *> *)imageDataCandidatesForURL:(NSURL *)url {
    if (!url.isFileURL) return @[];
    int fd = open(url.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return @[];
    vm_ipa_archive_t plan = {0};
    @try {
        struct stat before, after;
        if (fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_size <= 0 ||
            (uint64_t)before.st_size > 512u * 1024u * 1024u) return @[];
        static NSCache<NSString *, NSArray<NSData *> *> *cache;
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            cache = [NSCache new]; cache.countLimit = 128; cache.totalCostLimit = 16u * 1024u * 1024u;
        });
        NSString *key = IconIdentity(url, &before);
        NSArray<NSData *> *cached = [cache objectForKey:key];
        if (cached) return cached;
        vmfw_zip_t zip;
        if (vmfw_zip_open(&zip, IconRead, &fd, (uint64_t)before.st_size) != VMFW_ZIP_OK ||
            !vm_ipa_archive_open(&zip, &plan, NULL, 0)) return @[];
        NSData *plist = IconExtract(&zip, &plan.files[plan.content_indices[plan.info_index]], 1024u * 1024u);
        id parsed = plist ? [NSPropertyListSerialization propertyListWithData:plist
            options:NSPropertyListImmutable format:NULL error:NULL] : nil;
        if (![parsed isKindOfClass:NSDictionary.class]) return @[];
        NSDictionary *info = parsed;
        NSMutableOrderedSet<NSString *> *names = [NSMutableOrderedSet orderedSet];
        for (NSString *keyName in @[@"CFBundleIcons~iphone", @"CFBundleIcons"]) {
            id icons = info[keyName];
            id primary = [icons isKindOfClass:NSDictionary.class] ? icons[@"CFBundlePrimaryIcon"] : nil;
            if ([primary isKindOfClass:NSDictionary.class]) IconNames(names, primary[@"CFBundleIconFiles"]);
        }
        IconNames(names, info[@"CFBundleIconFiles"]);
        IconNames(names, info[@"CFBundleIconFile"]);
        // iPhone OS 2/3 apps may omit all icon keys and use the default filename.
        [names addObject:@"Icon.png"];
        NSMutableDictionary<NSString *, NSNumber *> *entries = [NSMutableDictionary dictionary];
        NSString *root = [NSString stringWithUTF8String:plan.app_root];
        for (unsigned i = 0; i < plan.count; i++) {
            NSString *path = [NSString stringWithUTF8String:plan.files[i].name];
            if (path && !plan.files[i].is_directory) entries[path.lowercaseString] = @(i);
        }
        NSMutableArray<NSData *> *images = [NSMutableArray array];
        NSMutableIndexSet *seen = [NSMutableIndexSet indexSet];
        NSUInteger total = 0;
        for (NSString *name in names) {
            NSNumber *index = entries[[root stringByAppendingPathComponent:name].lowercaseString];
            if (!index || [seen containsIndex:index.unsignedIntegerValue]) continue;
            [seen addIndex:index.unsignedIntegerValue];
            const vmfw_zip_entry_t *entry = &plan.files[plan.content_indices[index.unsignedIntValue]];
            if (entry->uncompressed_size > 4u * 1024u * 1024u - total) continue;
            NSData *data = IconExtract(&zip, entry, 2u * 1024u * 1024u);
            if (!data || !vm_ipa_icon_png_dimensions(data.bytes, data.length, NULL, NULL)) continue;
            [images addObject:data]; total += data.length;
            if (images.count == 8) break;
        }
        // Files can replace or edit an IPA while it is being inspected.
        if (fstat(fd, &after) || ![key isEqual:IconIdentity(url, &after)]) return @[];
        NSArray<NSData *> *result = [images copy];
        [cache setObject:result forKey:key cost:total];
        return result;
    } @finally {
        vm_ipa_archive_close(&plan);
        close(fd);
    }
}
@end
