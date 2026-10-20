// Native socket lifecycle tests with the real mux and a synthetic guest peer.
// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import <Foundation/Foundation.h>
#import "VMUSBTransport.h"
#include <dispatch/dispatch.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "line %d: %s\n", __LINE__, #x); } } while (0)

// These hardware entry points are not used: the test drives mux state directly.
bool usb_host_ready(const usb_host_t *host) { (void)host; return false; }
s5l_usb_result_t s5l8900_usb_packet(s5l8900_t *m, s5l_usb_token_t pid,
                                 unsigned ep, void *data, size_t capacity, size_t *actual) {
    (void)m; (void)pid; (void)ep; (void)data; (void)capacity; (void)actual;
    return S5L_USB_NAK;
}

@interface USBRequest : NSObject
@property (atomic) BOOL canceled;
@property int fd;
@property (nonatomic, strong) NSError *error;
@property (nonatomic, strong) dispatch_semaphore_t entered;
@property (nonatomic, strong) dispatch_semaphore_t done;
@end
@implementation USBRequest
- (void)dealloc { if (_fd >= 0) close(_fd); }
@end

static BOOL waitFor(dispatch_semaphore_t semaphore) {
    return dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0;
}
static USBRequest *request(VMUSBTransport *transport, uint16_t port,
                           NSTimeInterval timeout, BOOL canceled) {
    USBRequest *r = [USBRequest new]; r.fd = -1; r.canceled = canceled;
    r.entered = dispatch_semaphore_create(0); r.done = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
            __block BOOL notified = NO;
            NSError *error = nil;
            r.fd = [transport openPort:port timeout:timeout canceled:^BOOL{
                if (!notified) { notified = YES; dispatch_semaphore_signal(r.entered); }
                return r.canceled;
            } error:&error];
            r.error = error;
            if (!notified) dispatch_semaphore_signal(r.entered);
            dispatch_semaphore_signal(r.done);
        }
    });
    CHECK(waitFor(r.entered));
    return r;
}
static void failed(USBRequest *r, NSString *message) {
    CHECK(waitFor(r.done)); CHECK(r.fd < 0);
    CHECK([r.error.localizedDescription containsString:message]);
}
static unsigned openFDs(void) {
    unsigned count = 0;
    for (int fd = 0; fd < 1024; fd++) if (fcntl(fd, F_GETFD) >= 0) count++;
    return count;
}
static void ready(usb_mux_t *mux) {
    usb_mux_init(mux);
    size_t size = 0;
    CHECK(usb_mux_output(mux, &size) != NULL && size == 20);
    usb_mux_output_complete(mux);
    const uint8_t version[20] = {0,0,0,0, 0,0,0,20, 0,0,0,1};
    usb_mux_feed(mux, version, sizeof version);
    CHECK(usb_mux_ready(mux));
}
static int channelFor(usb_mux_t *mux, uint16_t port) {
    for (unsigned i = 0; i < USB_MUX_CHANNELS; i++)
        if (mux->channel[i].remote_port == port &&
            mux->channel[i].state != USB_MUX_CLOSED && mux->channel[i].state != USB_MUX_UNUSED)
            return (int)i;
    return -1;
}
static int acceptRequest(VMUSBTransport *transport, usb_mux_t *mux, USBRequest *r, uint16_t port) {
    [transport pollWithMux:mux];
    int channel = channelFor(mux, port); CHECK(channel >= 0);
    if (channel < 0) { r.canceled = YES; failed(r, @"canceled"); return -1; }
    mux->channel[channel].state = USB_MUX_OPEN;
    [transport pollWithMux:mux];
    CHECK(waitFor(r.done)); CHECK(r.fd >= 0 && r.error == nil);
    return channel;
}
static void closeClient(VMUSBTransport *transport, usb_mux_t *mux, USBRequest *r) {
    if (r.fd >= 0) close(r.fd); r.fd = -1;
    [transport pollWithMux:mux]; [transport pollWithMux:mux];
    size_t size;
    // Flush each synthetic RST, making closed channels reusable.
    for (unsigned i = 0; i < USB_MUX_CHANNELS && usb_mux_output(mux, &size); i++)
        usb_mux_output_complete(mux);
}
static void testBeforeReady(VMUSBTransport *transport, usb_mux_t *mux) {
    [transport beginSession]; usb_mux_init(mux);
    CHECK(![transport hasServiceActivity]);
    failed(request(transport, 62078, 1, YES), @"canceled");
    unsigned baseline = openFDs();
    for (unsigned i = 0; i < USB_MUX_CHANNELS * 3; i++) {
        // No emulator poll at all: canceled, unassigned requests release now.
        failed(request(transport, 62078, 1, YES), @"canceled");
        failed(request(transport, 62078, 0.01, NO), @"did not respond in time");
        CHECK(![transport hasServiceActivity]);
        [transport pollWithMux:mux];
        CHECK(openFDs() == baseline);
    }
    ready(mux);
    USBRequest *r = request(transport, 62078, 2, NO);
    CHECK([transport hasServiceActivity]);
    int channel = acceptRequest(transport, mux, r, 62078);
    if (channel >= 0 && r.fd >= 0) {
        CHECK(send(r.fd, "ping", 4, 0) == 4); [transport pollWithMux:mux];
        CHECK(mux->channel[channel].send_used == 4);
        CHECK(!memcmp(mux->channel[channel].send, "ping", 4));
    }
    closeClient(transport, mux, r);
    CHECK(![transport hasServiceActivity]);
    CHECK(openFDs() == baseline);
}
static void testAssignedCancellation(VMUSBTransport *transport, usb_mux_t *mux) {
    [transport beginSession]; ready(mux);
    USBRequest *live = request(transport, 62078, 2, NO);
    int liveChannel = acceptRequest(transport, mux, live, 62078);
    USBRequest *pending = request(transport, 62079, 2, NO);
    [transport pollWithMux:mux];
    int pendingChannel = channelFor(mux, 62079); CHECK(pendingChannel >= 0);
    unsigned activeStage = mux->stage; mux->stage = 0;
    pending.canceled = YES; failed(pending, @"canceled");
    // The channel must close even though usb_mux_ready is now false.
    [transport pollWithMux:mux];
    if (pendingChannel >= 0) CHECK(mux->channel[pendingChannel].state == USB_MUX_CLOSED);
    if (liveChannel >= 0) CHECK(mux->channel[liveChannel].state == USB_MUX_OPEN);
    CHECK(mux->error == NULL); mux->stage = activeStage;
    if (live.fd >= 0 && liveChannel >= 0) {
        CHECK(send(live.fd, "ok", 2, 0) == 2); [transport pollWithMux:mux];
        CHECK(mux->channel[liveChannel].send_used == 2);
    }
    USBRequest *retry = request(transport, 62079, 2, NO);
    acceptRequest(transport, mux, retry, 62079);
    closeClient(transport, mux, retry); closeClient(transport, mux, live);
}
static void testCloseErrorAndReset(VMUSBTransport *transport, usb_mux_t *mux) {
    [transport beginSession]; ready(mux);
    USBRequest *r = request(transport, 62078, 2, NO);
    int channel = acceptRequest(transport, mux, r, 62078);
    if (channel >= 0 && r.fd >= 0) {
        // Preserve a final reply delivered in the same USB batch as closure.
        memcpy(mux->channel[channel].receive, "done", 4);
        mux->channel[channel].receive_used = 4;
        mux->channel[channel].state = USB_MUX_CLOSED;
        [transport pollWithMux:mux];
        char buffer[8] = {0}; CHECK(recv(r.fd, buffer, sizeof buffer, 0) == 4);
        CHECK(!memcmp(buffer, "done", 4)); CHECK(recv(r.fd, buffer, sizeof buffer, 0) == 0);
    }
    closeClient(transport, mux, r);
    r = request(transport, 62079, 2, NO); [transport pollWithMux:mux];
    channel = channelFor(mux, 62079); CHECK(channel >= 0);
    if (channel >= 0) {
        mux->channel[channel].state = USB_MUX_ERROR; mux->channel[channel].error = "Service refused";
        [transport pollWithMux:mux]; failed(r, @"Service refused");
    } else { r.canceled = YES; failed(r, @"canceled"); }
    [transport beginSession]; usb_mux_init(mux);
    r = request(transport, 62078, 2, NO);
    [transport beginSession]; failed(r, @"new USB session");
    ready(mux);
    r = request(transport, 62078, 2, NO); [transport pollWithMux:mux];
    usb_mux_cancel(mux, "Link failed"); [transport pollWithMux:mux]; failed(r, @"Link failed");
    [transport beginSession]; ready(mux);
    r = request(transport, 62078, 2, NO); acceptRequest(transport, mux, r, 62078);
    [transport endSession:@"Stopped"];
    if (r.fd >= 0) { char byte; CHECK(recv(r.fd, &byte, 1, 0) == 0); }
    CHECK(![transport hasServiceActivity]);
    failed(request(transport, 62078, 2, NO), @"Stopped");
}
static void testReconnectWaitingAndLiveStreams(VMUSBTransport *transport, usb_mux_t *mux) {
    [transport beginSession]; usb_mux_init(mux);
    // A new request during saved-state cable recovery must survive reconnect,
    // carry wake demand even before enumeration, and finish on the new link.
    USBRequest *waiting = request(transport, 64321, 3, NO);
    CHECK([transport hasServiceActivity]);
    [transport reconnectSession:@"Saved cable reconnect"];
    usb_mux_cancel(mux, "Handshake failed"); [transport pollWithMux:mux];
    CHECK([transport hasServiceActivity]);
    ready(mux);
    acceptRequest(transport, mux, waiting, 64321);
    CHECK([transport hasServiceActivity]);
    // Sleep must not make an already closed host socket a permanent wake lease.
    close(waiting.fd); waiting.fd = -1;
    CHECK(![transport hasServiceActivity]);
    [transport pollWithMux:mux];

    [transport beginSession]; ready(mux);
    USBRequest *live = request(transport, 62078, 3, NO);
    acceptRequest(transport, mux, live, 62078);
    USBRequest *assigned = request(transport, 62079, 3, NO);
    [transport pollWithMux:mux];
    waiting = request(transport, 64321, 3, NO);
    [transport reconnectSession:@"Cable changed"];
    failed(assigned, @"Cable changed");
    char byte;
    CHECK(recv(live.fd, &byte, 1, 0) == 0); // Never replay an in-flight operation.
    CHECK([transport hasServiceActivity]);
    ready(mux); acceptRequest(transport, mux, waiting, 64321);
    closeClient(transport, mux, waiting);
    CHECK(![transport hasServiceActivity]);

    [transport beginSession]; usb_mux_init(mux);
    waiting = request(transport, 64321, 0.05, NO);
    [transport reconnectSession:@"Retry"];
    failed(waiting, @"did not respond in time"); // Deadline was not extended.
    CHECK(![transport hasServiceActivity]);
    waiting = request(transport, 64321, 3, NO);
    waiting.canceled = YES; failed(waiting, @"canceled");
    [transport reconnectSession:@"Retry"];
    CHECK(![transport hasServiceActivity]);
}
int main(void) {
    @autoreleasepool {
        VMUSBTransport *transport = [VMUSBTransport new];
        usb_mux_t *mux = calloc(1, sizeof *mux);
        if (!mux) return 1;
        testBeforeReady(transport, mux);
        testAssignedCancellation(transport, mux);
        testCloseErrorAndReset(transport, mux);
        testReconnectWaitingAndLiveStreams(transport, mux);
        [transport endSession:@"Finished"]; free(mux);
        printf("VMUSBTransport: %u checks, %u failures\n", checks, failures);
        return failures ? 1 : 0;
    }
}
