/* In-process virtual USB host. No physical USB, sockets or guest disk access. */
#ifndef S5LBOX_USB_HOST_H
#define S5LBOX_USB_HOST_H
#include "soc.h"

typedef struct {
    unsigned stage, control, used, wanted, config_index, config_count;
    uint32_t reset_generation;
    uint8_t setup[8], descriptor[4096];
    uint8_t configuration, bulk_in, bulk_out;
    uint16_t vendor, product;
    const char *error; /* host-only; this object is never snapshotted */
} usb_host_t;

/* The caller must cold-boot with the USB device-tree node matched. */
void usb_host_init(usb_host_t *h, s5l8900_t *m);
/* Poll between guest batches. No blocking waits. A caller owns its deadline. */
void usb_host_poll(usb_host_t *h, s5l8900_t *m);
bool usb_host_ready(const usb_host_t *h);
/* A bus reset has started enumeration. Waiting for the guest driver, a ready
 * link, and terminal errors are not enumeration. */
bool usb_host_enumerating(const usb_host_t *h);
void usb_host_disconnect(usb_host_t *h, s5l8900_t *m);
#endif
