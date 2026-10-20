/* Synthetic archives only. Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "VMIPAArchive.h"
#include "VMFirmwareFixtures.h"
#include "VMFirmwareTest.h"

static uint8_t archive[8192];
static size_t archive_size;
static size_t read_archive(void *ctx, uint64_t offset, uint8_t *out, size_t count) {
    (void)ctx;
    if (offset > archive_size || count > archive_size - offset) return 0;
    memcpy(out, archive + offset, count);
    return count;
}
static bool inspect(void) {
    vmfw_zip_t zip;
    vm_ipa_archive_t plan = {0};
    char detail[256];
    if (vmfw_zip_open(&zip, read_archive, NULL, archive_size) != VMFW_ZIP_OK) return false;
    bool ok = vm_ipa_archive_open(&zip, &plan, detail, sizeof detail);
    if (ok) vm_ipa_archive_close(&plan);
    return ok;
}
static void macho(uint8_t *b, uint32_t subtype) {
    memset(b, 0, 64);
    fx_w32le(b, 0xfeedface); fx_w32le(b + 4, 12);
    fx_w32le(b + 8, subtype); fx_w32le(b + 12, 2);
}
static void set_mode(const fx_zip_layout_t *layout, unsigned index, unsigned mode) {
    fx_w16le(archive + layout->central_offset[index] + 4, (3u << 8) | 20u);
    fx_w32le(archive + layout->central_offset[index] + 38, mode << 16);
}
static void links(vmfw_test_t *t) {
    VMFW_T_SECTION(t, "bounded internal IPA file links");
    static const uint8_t content[] = "test";
    static const uint8_t target[] = "_CodeSignature/CodeResources";
    fx_zip_member_t members[5] = {
        {"Payload/Test.app/Info.plist", content, 4, false, false},
        {"Payload/Test.app/CodeResources", target, sizeof target - 1, true, true},
        {"Payload/Test.app/_CodeSignature/CodeResources", content, 4, false, false},
        {"Payload/Test.app/Alias", (const uint8_t *)"CodeResources", 13, false, false},
        {"Payload/Test.app/Other", content, 4, false, false}
    };
    fx_zip_layout_t layout;
    archive_size = fx_zip_build(archive, sizeof archive, members, 5, &layout);
    set_mode(&layout, 1, 0120755u);
    set_mode(&layout, 3, 0120777u);
    VMFW_T_CHECK(t, inspect(), "legacy signature link and chained alias refused");
    vmfw_zip_t zip;
    vm_ipa_archive_t plan = {0};
    char detail[256];
    VMFW_T_CHECK(t, vmfw_zip_open(&zip, read_archive, NULL, archive_size) == VMFW_ZIP_OK &&
        vm_ipa_archive_open(&zip, &plan, detail, sizeof detail), "link plan refused");
    if (plan.files) {
        VMFW_T_CHECK(t, plan.content_indices[0] == 0 && plan.content_indices[1] == 2 &&
            plan.content_indices[3] == 2 && vm_ipa_entry_is_symlink(&plan.files[1]) &&
            plan.files[1].uncompressed_size == sizeof target - 1,
            "links must resolve for inspection but preserve original install data");
        vm_ipa_archive_close(&plan);
    }
    const char *bad[] = {"", "/etc/passwd", "../outside", "../../Other.app/file", "./Other",
        "a/../Other", "a//Other", "a\\Other", "a:Other", "a\001Other", "missing",
        "CodeResources", "Alias", "_CodeSignature", "_CodeSignature/", "other"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        members[1].data = (const uint8_t *)bad[i]; members[1].len = strlen(bad[i]);
        archive_size = fx_zip_build(archive, sizeof archive, members, 5, &layout);
        set_mode(&layout, 1, 0120777u); set_mode(&layout, 3, 0120777u);
        VMFW_T_CHECK(t, !inspect(), "unsafe, missing or cyclic link accepted: %s", bad[i]);
    }
    static const uint8_t nul_target[] = "Other\0ignored";
    members[1].data = nul_target; members[1].len = sizeof nul_target - 1;
    archive_size = fx_zip_build(archive, sizeof archive, members, 5, &layout);
    set_mode(&layout, 1, 0120777u);
    VMFW_T_CHECK(t, !inspect(), "embedded NUL link accepted");
    members[1].data = target; members[1].len = sizeof target - 1;
    archive_size = fx_zip_build(archive, sizeof archive, members, 5, &layout);
    set_mode(&layout, 1, 0120777u);
    archive[layout.local_offset[1] + 30 + strlen(members[1].name) + 6] ^= 1;
    VMFW_T_CHECK(t, !inspect(), "corrupt compressed link accepted");
    archive_size = fx_zip_build(archive, sizeof archive, members, 5, &layout);
    set_mode(&layout, 1, 0120777u);
    fx_w32le(archive + layout.central_offset[1] + 24, VMFW_ZIP_MAX_NAME);
    VMFW_T_CHECK(t, !inspect(), "oversized link accepted");
    const unsigned special[] = {0010644u, 0020644u, 0060644u, 0140644u, 0040755u};
    for (size_t i = 0; i < sizeof special / sizeof special[0]; i++) {
        archive_size = fx_zip_build(archive, sizeof archive, members, 5, &layout);
        set_mode(&layout, 1, special[i]);
        VMFW_T_CHECK(t, !inspect(), "special file or inconsistent directory accepted");
    }
    const char *children[] = {"Payload/Test.app/CodeResources/child", "Payload/Test.app/coderesources/child"};
    for (size_t i = 0; i < sizeof children / sizeof children[0]; i++) {
        members[4].name = children[i];
        archive_size = fx_zip_build(archive, sizeof archive, members, 5, &layout);
        set_mode(&layout, 1, 0120777u);
        VMFW_T_CHECK(t, !inspect(), "symlink extraction parent accepted");
        set_mode(&layout, 1, 0100644u);
        VMFW_T_CHECK(t, !inspect(), "regular file extraction parent accepted");
    }
    members[4].name = "Payload/Test.app/Other";
    /* A linked plist must resolve to real content, not parse its path as XML. */
    members[0].data = (const uint8_t *)"Other"; members[0].len = 5;
    archive_size = fx_zip_build(archive, sizeof archive, members, 5, &layout);
    set_mode(&layout, 0, 0120777u); set_mode(&layout, 1, 0120777u);
    VMFW_T_CHECK(t, inspect(), "linked Info.plist refused");
    vm_ipa_archive_t info_plan = {0};
    if (vmfw_zip_open(&zip, read_archive, NULL, archive_size) == VMFW_ZIP_OK &&
        vm_ipa_archive_open(&zip, &info_plan, detail, sizeof detail)) {
        VMFW_T_CHECK(t, info_plan.content_indices[info_plan.info_index] == 4, "linked plist not resolved");
        vm_ipa_archive_close(&info_plan);
    }
}
int main(void) {
    vmfw_test_t state = {0}, *t = &state;
    char detail[256];
    VMFW_T_SECTION(t, "IPA paths and archive policy");
    const char *bad[] = {"", "/a", "../a", "a/../b", "a/./b", "a//b", "a\\b", "a:b", "a\001b"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        VMFW_T_CHECK(t, !vm_ipa_safe_path(bad[i]), "unsafe path accepted: %s", bad[i]);
    VMFW_T_CHECK(t, vm_ipa_safe_path("Payload/Test.app/data/"), "valid path refused");
    static const uint8_t content[] = "test";
    fx_zip_member_t members[3] = {
        {"Payload/Test.app/Info.plist", content, 4, false, false},
        {"Payload/Test.app/Test", content, 4, true, true},
        {"iTunesMetadata.plist", content, 4, false, false}
    };
    fx_zip_layout_t layout;
    archive_size = fx_zip_build(archive, sizeof archive, members, 3, &layout);
    VMFW_T_CHECK(t, inspect(), "valid bundle refused");
    fx_w16le(archive + layout.cd_offset + 4, (3u << 8) | 20u);
    fx_w32le(archive + layout.cd_offset + 38, 0120777u << 16);
    VMFW_T_CHECK(t, !inspect(), "symlink accepted");
    const char *conflicts[] = {"Payload/Test.app/info.plist", "Payload/Other.app/Info.plist", "Payload/Test.app/../oops"};
    for (size_t i = 0; i < sizeof conflicts / sizeof conflicts[0]; i++) {
        members[1].name = conflicts[i];
        archive_size = fx_zip_build(archive, sizeof archive, members, 3, &layout);
        VMFW_T_CHECK(t, !inspect(), "bad member accepted: %s", conflicts[i]);
    }
    members[1].name = "Payload/Test.app/Test";
    archive_size = fx_zip_build(archive, sizeof archive, members, 3, &layout);
    fx_w32le(archive + layout.cd_offset + 24, VM_IPA_MAX_FILE_BYTES + 1);
    VMFW_T_CHECK(t, !inspect(), "oversize file accepted");

    links(t);

    VMFW_T_SECTION(t, "Mach-O compatibility");
    uint8_t thin[64], fat[256];
    macho(thin, 6);
    VMFW_T_CHECK(t, vm_ipa_check_executable(thin, 28, detail, sizeof detail), "ARMv6 refused: %s", detail);
    fx_w32le(thin + 8, 9);
    VMFW_T_CHECK(t, !vm_ipa_check_executable(thin, 28, detail, sizeof detail), "ARMv7 accepted");
    macho(thin, 6);
    fx_w32le(thin, 0xfeedfacf);
    VMFW_T_CHECK(t, !vm_ipa_check_executable(thin, 28, detail, sizeof detail), "ARM64 accepted");
    macho(thin, 6);
    fx_w32le(thin + 16, 1); fx_w32le(thin + 20, 20);
    fx_w32le(thin + 28, 0x21); fx_w32le(thin + 32, 20); fx_w32le(thin + 44, 1);
    VMFW_T_CHECK(t, !vm_ipa_check_executable(thin, 48, detail, sizeof detail), "encrypted executable accepted");
    fx_w32le(thin + 44, 0);
    VMFW_T_CHECK(t, vm_ipa_check_executable(thin, 48, detail, sizeof detail), "unencrypted executable refused");
    fx_w32le(thin + 20, 16); fx_w32le(thin + 28, 0x25); fx_w32le(thin + 32, 16);
    fx_w32le(thin + 36, 0x00040000);
    VMFW_T_CHECK(t, !vm_ipa_check_executable(thin, 44, detail, sizeof detail), "iOS4 minimum accepted");
    fx_w32le(thin + 36, 0x00030103);
    VMFW_T_CHECK(t, vm_ipa_check_executable(thin, 44, detail, sizeof detail), "OS3 minimum refused");
    fx_w32le(thin + 32, 80);
    VMFW_T_CHECK(t, !vm_ipa_check_executable(thin, 44, detail, sizeof detail), "command overflow accepted");
    memset(fat, 0, sizeof fat);
    fx_w32be(fat, 0xcafebabe); fx_w32be(fat + 4, 2);
    for (unsigned i = 0; i < 2; i++) {
        uint8_t *a = fat + 8 + i * 20;
        fx_w32be(a, 12); fx_w32be(a + 4, i ? 9 : 6);
        fx_w32be(a + 8, 64 + i * 64); fx_w32be(a + 12, 28);
        macho(fat + 64 + i * 64, i ? 9 : 6);
    }
    VMFW_T_CHECK(t, vm_ipa_check_executable(fat, sizeof fat, detail, sizeof detail), "valid universal refused");
    fx_w32be(fat + 36, 250);
    VMFW_T_CHECK(t, !vm_ipa_check_executable(fat, sizeof fat, detail, sizeof detail), "bad later slice ignored");
    fx_w32be(fat + 36, 64);
    VMFW_T_CHECK(t, !vm_ipa_check_executable(fat, sizeof fat, detail, sizeof detail), "overlapping slices accepted");
    fx_w32be(fat + 36, 128); fx_w32be(fat + 32, 6);
    VMFW_T_CHECK(t, !vm_ipa_check_executable(fat, sizeof fat, detail, sizeof detail), "mismatched architecture accepted");
    VMFW_T_SECTION(t, "bounded native icon decoding input");
    uint8_t png[64] = {137,80,78,71,13,10,26,10};
    fx_w32be(png + 8, 13); memcpy(png + 12, "IHDR", 4);
    fx_w32be(png + 16, 57); fx_w32be(png + 20, 60);
    png[24] = 8; png[25] = 6;
    uint32_t width = 0, height = 0;
    VMFW_T_CHECK(t, vm_ipa_icon_png_dimensions(png, 33, &width, &height) && width == 57 && height == 60,
        "valid legacy icon dimensions refused");
    for (size_t n = 0; n < 33; n++)
        VMFW_T_CHECK(t, !vm_ipa_icon_png_dimensions(png, n, NULL, NULL), "truncated PNG accepted");
    fx_w32be(png + 16, 2049);
    VMFW_T_CHECK(t, !vm_ipa_icon_png_dimensions(png, 33, NULL, NULL), "oversize width accepted");
    fx_w32be(png + 16, 0);
    VMFW_T_CHECK(t, !vm_ipa_icon_png_dimensions(png, 33, NULL, NULL), "zero width accepted");
    fx_w32be(png + 16, 57); fx_w32be(png + 20, UINT32_MAX);
    VMFW_T_CHECK(t, !vm_ipa_icon_png_dimensions(png, 33, NULL, NULL), "oversize height accepted");
    fx_w32be(png + 20, 60); png[28] = 2;
    VMFW_T_CHECK(t, !vm_ipa_icon_png_dimensions(png, 33, NULL, NULL), "bad interlace accepted");
    png[28] = 0;
    VMFW_T_CHECK(t, !vm_ipa_icon_png_dimensions(png, 2u * 1024u * 1024u + 1, NULL, NULL), "oversize icon accepted");
    memmove(png + 24, png + 8, 25);
    fx_w32be(png + 8, 4); memcpy(png + 12, "CgBI", 4); memset(png + 16, 0, 8);
    VMFW_T_CHECK(t, vm_ipa_icon_png_dimensions(png, 49, &width, &height) && width == 57 && height == 60,
        "Apple optimized icon header refused");
    VMFW_T_CHECK(t, !vm_ipa_icon_png_dimensions(png, 48, NULL, NULL), "truncated optimized icon accepted");
    fx_w32be(png + 8, UINT32_MAX);
    VMFW_T_CHECK(t, !vm_ipa_icon_png_dimensions(png, 49, NULL, NULL), "CgBI length overflow accepted");
    VMFW_T_CHECK(t, !vm_ipa_icon_png_dimensions(NULL, 49, NULL, NULL), "null icon accepted");
    printf("IPA inspection: %u checks, %u failures\n", t->checks, t->failures);
    return t->failures ? 1 : 0;
}
