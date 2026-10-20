#include "VMUSBRecovery.h"
#include <string.h>

void vm_usb_recovery_reset(vm_usb_recovery_t *s) {
    memset(s, 0, sizeof *s);
}
void vm_usb_recovery_resume(vm_usb_recovery_t *s, double seconds) {
    if (seconds <= 0) return;
    if (s->negotiating) s->negotiation_start += seconds;
    if (s->disconnected) s->retry_at += seconds;
    if (s->was_ready) s->ready_start += seconds;
}
vm_usb_recovery_action_t vm_usb_recovery_poll(vm_usb_recovery_t *s,
    double now, double guest, bool negotiating, bool failed, bool ready) {
    if (s->disconnected) {
        /* A same-chunk unplug/replug does not let the guest retire stale USB
         * state. Require both backoff and guest time with the cable absent. */
        if (now < s->retry_at || guest - s->detached_guest_time < 0.25)
            return VM_USB_KEEP_LINK;
        s->disconnected = false;
        return VM_USB_RECONNECT;
    }
    if (ready && !failed) {
        s->negotiating = false;
        if (!s->was_ready) { s->was_ready = true; s->ready_start = now; }
        if (now - s->ready_start >= 30.0) s->failures = 0;
        return VM_USB_KEEP_LINK;
    }
    s->was_ready = false;
    if (negotiating && !s->negotiating) s->negotiation_start = now;
    s->negotiating = negotiating;
    if (!failed && (!negotiating || now - s->negotiation_start < 120.0))
        return VM_USB_KEEP_LINK;
    if (s->failures < 5) s->failures++;
    double delay = (double)(1u << s->failures);
    if (delay > 30.0) delay = 30.0;
    s->retry_at = now + delay;
    s->detached_guest_time = guest;
    s->disconnected = true;
    s->negotiating = false;
    return VM_USB_DISCONNECT;
}
