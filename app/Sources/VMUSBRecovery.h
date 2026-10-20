/* Host-only USB cable recovery; never serialized into guest snapshots. */
#ifndef VM_USB_RECOVERY_H
#define VM_USB_RECOVERY_H
#include <stdbool.h>

typedef struct {
    double negotiation_start, retry_at, detached_guest_time, ready_start;
    unsigned failures;
    bool negotiating, disconnected, was_ready;
} vm_usb_recovery_t;

typedef enum {
    VM_USB_KEEP_LINK, VM_USB_DISCONNECT, VM_USB_RECONNECT
} vm_usb_recovery_action_t;

void vm_usb_recovery_reset(vm_usb_recovery_t *state);
/* Exclude host pauses from negotiation, backoff and stable-link deadlines. */
void vm_usb_recovery_resume(vm_usb_recovery_t *state, double paused_seconds);
/* Only whole-link failures belong here, never an individual service refusal.
 * guest_seconds must advance through real guest execution before reconnect. */
vm_usb_recovery_action_t vm_usb_recovery_poll(vm_usb_recovery_t *state,
    double active_now, double guest_seconds, bool negotiating, bool failed,
    bool ready);
#endif
