// Guest-owned AFC endpoint. No host listener or bundled Apple executable.
// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#ifndef S5LBOX_GUEST_FILES_SERVICE_H
#define S5LBOX_GUEST_FILES_SERVICE_H
#define VM_GUEST_FILES_PORT 62080
#define VM_GUEST_FILES_PLIST_PATH "/System/Library/LaunchDaemons/com.j0shua.s5lbox.files-v1.plist"
#define VM_GUEST_FILES_PLIST \
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" \
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n" \
    "<plist version=\"1.0\"><dict>" \
    "<key>Label</key><string>com.j0shua.s5lbox.files</string>" \
    "<key>ProgramArguments</key><array><string>/usr/libexec/afcd</string><string>--launchd</string><string>-d</string><string>/</string></array>" \
    "<key>ServiceIPC</key><true/><key>TimeOut</key><integer>30</integer>" \
    "<key>Sockets</key><dict><key>Listeners</key><dict>" \
    "<key>SockNodeName</key><string>127.0.0.1</string>" \
    "<key>SockServiceName</key><string>62080</string>" \
    "<key>SockFamily</key><string>IPv4</string><key>SockType</key><string>stream</string>" \
    "</dict></dict></dict></plist>\n"
#endif
