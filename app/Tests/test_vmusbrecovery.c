#include "VMUSBRecovery.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "USB recovery failed at line %d: %s\n", __LINE__, #c); \
    return EXIT_FAILURE; } } while (0)

int main(void) {
    vm_usb_recovery_t s;
    vm_usb_recovery_reset(&s);
    /* Waiting for the guest driver is not a failed link. */
    CHECK(vm_usb_recovery_poll(&s, 1000, 10, false, false, false) == VM_USB_KEEP_LINK);
    CHECK(vm_usb_recovery_poll(&s, 1000, 10, true, false, false) == VM_USB_KEEP_LINK);
    vm_usb_recovery_resume(&s, 600);
    CHECK(vm_usb_recovery_poll(&s, 1719, 11, true, false, false) == VM_USB_KEEP_LINK);
    CHECK(vm_usb_recovery_poll(&s, 1720, 11, true, false, false) == VM_USB_DISCONNECT);
    CHECK(s.failures == 1 && s.disconnected);
    CHECK(vm_usb_recovery_poll(&s, 1721, 12, false, false, false) == VM_USB_KEEP_LINK);
    /* Wall time alone cannot reconnect while the guest made no progress. */
    CHECK(vm_usb_recovery_poll(&s, 1722, 11, false, false, false) == VM_USB_KEEP_LINK);
    vm_usb_recovery_resume(&s, 600);
    CHECK(vm_usb_recovery_poll(&s, 2321, 12, false, false, false) == VM_USB_KEEP_LINK);
    CHECK(vm_usb_recovery_poll(&s, 2322, 12, false, false, false) == VM_USB_RECONNECT);
    CHECK(!s.disconnected);
    CHECK(vm_usb_recovery_poll(&s, 2323, 13, false, true, false) == VM_USB_DISCONNECT);
    CHECK(s.retry_at == 2327 && s.failures == 2);
    CHECK(vm_usb_recovery_poll(&s, 2327, 14, false, false, false) == VM_USB_RECONNECT);
    CHECK(vm_usb_recovery_poll(&s, 2328, 14, false, false, true) == VM_USB_KEEP_LINK);
    CHECK(vm_usb_recovery_poll(&s, 2357, 15, false, false, true) == VM_USB_KEEP_LINK);
    CHECK(s.failures == 2);
    CHECK(vm_usb_recovery_poll(&s, 2358, 15, false, false, true) == VM_USB_KEEP_LINK);
    CHECK(s.failures == 0);
    s.failures = 2;
    vm_usb_recovery_resume(&s, 600);
    CHECK(vm_usb_recovery_poll(&s, 2957, 15, false, false, true) == VM_USB_KEEP_LINK);
    CHECK(s.failures == 2);
    CHECK(vm_usb_recovery_poll(&s, 2958, 15, false, false, true) == VM_USB_KEEP_LINK);
    CHECK(s.failures == 0);
    /* Healthy service failures/cancellations are not link failures. */
    CHECK(vm_usb_recovery_poll(&s, 9999, 16, false, false, true) == VM_USB_KEEP_LINK);
    double now = 10000, guest = 17;
    for (unsigned i = 0; i < 20; i++) {
        CHECK(vm_usb_recovery_poll(&s, now, guest, false, true, false) == VM_USB_DISCONNECT);
        CHECK(s.retry_at > now && s.retry_at <= now + 30);
        now = s.retry_at; guest += 1;
        CHECK(vm_usb_recovery_poll(&s, now, guest, false, false, false) == VM_USB_RECONNECT);
    }
    CHECK(s.failures == 5);
    /* A real guest reboot resets clocks and the host-only recovery state. */
    vm_usb_recovery_reset(&s);
    CHECK(!s.disconnected && !s.negotiating && s.failures == 0);
    CHECK(vm_usb_recovery_poll(&s, 0, 0, true, false, false) == VM_USB_KEEP_LINK);
    CHECK(vm_usb_recovery_poll(&s, 1, 1, false, false, true) == VM_USB_KEEP_LINK);
    puts("USB recovery policy passed (timeout, pause, backoff, success, reboot)");
    return EXIT_SUCCESS;
}
