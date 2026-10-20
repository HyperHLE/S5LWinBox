//
//  S5LBox — VMManualViewController. See the header.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMManualViewController.h"

#import "VMSettings.h"

/*
 * The manual, as data. Each entry is a heading and a body; the table renders
 * them and does nothing else. Kept here rather than in a bundled RTF or a web
 * view because the app ships no resources beyond an icon, and a manual that
 * cannot be read without a network is not a manual.
 */
typedef struct {
    __unsafe_unretained NSString *heading;
    __unsafe_unretained NSString *body;
} vm_manual_entry_t;

@implementation VMManualViewController {
    NSArray<NSDictionary<NSString *, NSString *> *> *_entries;
}

- (instancetype)init {
    self = [super initWithStyle:UITableViewStyleInsetGrouped];
    if (!self) return nil;
    self.title = @"Manual";
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.tableView.allowsSelection = NO;
    [self rebuild];

    self.navigationItem.rightBarButtonItem =
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                      target:self
                                                      action:@selector(dismissSelf)];

    [[NSNotificationCenter defaultCenter]
        addObserver:self selector:@selector(rebuild)
               name:VMSettingsDidChangeNotification object:nil];
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (void)dismissSelf {
    if (self.presentingViewController)
        [self dismissViewControllerAnimated:YES completion:nil];
    else
        [self.navigationController popViewControllerAnimated:YES];
}

/* The last two sections only exist in developer mode: somebody who has not
 * turned it on has no switches to be confused by and no console to find. */
- (void)rebuild {
    BOOL dev = [[VMSettings sharedSettings] developerMode];
    NSMutableArray *e = [NSMutableArray array];

    [e addObject:@{ @"h": @"What this is",
        @"b": @"S5LBox emulates the iPhone 3G to run iPhone OS 3.1.3, "
              @"including its apps, Home screen and Cydia. Download or import firmware "
              @"during setup, then open a machine to get started." }];

    [e addObject:@{ @"h": @"What this app does today",
        @"b": @"You can run multiple saved machines, install compatible IPAs "
              @"and manage tweaks through Cydia or S5LBox Packages. "
              @"Compatibility and speed vary by app and device. Without "
              @"firmware, a machine opens the built-in test program." }];

    [e addObject:@{ @"h": @"Machines",
        @"b": @"The first screen is a list of machines. Each one has its own "
              @"disk — its own writable copy of the filesystem, made the first "
              @"time you open it — so what one machine does to iPhone OS does "
              @"not happen to the others.\n\nWhat they SHARE is the firmware "
              @"you imported, which nothing ever writes to, and App Settings "
              @"defaults. Machine Settings controls only the open session.\n\n"
              @"Touch and hold a machine for Rename, Duplicate and Delete. Duplicate "
              @"copies the settings, not the disk: the copy builds its own the "
              @"first time you open it. Delete removes that machine's disk too, "
              @"which is most of the space it uses.\n\n"
              @"The gear on this screen opens App Settings. Firmware import is "
              @"under Advanced. Tap a machine to open it."
              @"\n\nNew machines have 8 GB of storage, using space as needed. "
              @"Choose a smaller limit under Advanced before creating a machine. "
              @"Existing machines keep their size."
              @"\n\nOne machine can run at a time." }];

    [e addObject:@{ @"h": @"The screen and the buttons",
        @"b": @"Below the display are Machine Settings, "
              @"Home, and Device Controls. Home supports a press or a hold. "
              @"Device Controls contains Sleep / Wake (hold for the guest power "
              @"menu), Silent mode, volume, and session actions. Pause freezes "
              @"emulation; it is different from putting the guest display to sleep. "
              @"Choose Home only under App Settings → Advanced → Emulator interface "
              @"to hide the extra controls. Tap outside the guest display to show "
              @"them again; VoiceOver offers the same action on Home. "
              @"App Settings → Advanced → Emulator interface → Legacy restores the original "
              @"five-button row, diagnostics, and toolbar.\n\n"
              @"The guest must have loaded its input drivers to respond. "
              @"Developer Mode exposes delivery diagnostics in Console." }];

    [e addObject:@{ @"h": @"Firmware",
        @"b": @"Setup can download iPhone OS 3.1.3 for iPhone 3G from Apple, "
              @"or import an IPSW from Files. It verifies and prepares the firmware "
              @"automatically. Each machine creates its own disk "
              @"from the prepared firmware on first boot." }];

    [e addObject:@{ @"h": @"Machine Settings",
        @"b": @"Settings inside an open machine lets you browse saved states, "
              @"save and close, shut down, or restart. Pause is in Device Controls. "
              @"Microphone and background pause are under Advanced. Restart "
              @"asks first because unsaved guest work may be lost.\n\n"
              @"Developer Mode adds an inline console and instruction limit "
              @"for this session. These changes do not rewrite App Settings; "
              @"opening a machine again starts with the app defaults. Firmware, "
              @"jailbreak and new-machine setup stay in App Settings." }];

    [e addObject:@{ @"h": @"Install a guest app",
        @"b": @"Inside a running machine, open Settings → Install IPA. "
              @"Copy IPAs into Files → On My iPhone → "
              @"S5LBox → IPAs, or use Import in the library. Each app has its "
              @"own Install button; adding files never installs them automatically. "
              @"Select an unencrypted ARMv6 IPA built for "
              @"iPhone OS 3.1.3 or earlier. Virtual USB sends it to the guest's "
              @"built-in installer without shutting down. The guest registers "
              @"the app and may update an existing app with the same ID.\n\n"
              @"Unsigned homebrew needs compatible AppSync inside the guest "
              @"(AppSync for OS 3.1 on iPhone OS 3.1.3). Virtual USB is on for "
              @"fresh boots unless explicitly disabled; older saved sessions "
              @"may need a full shutdown and fresh boot.\n\n"
              @"From Machines, App Settings → Install IPA retains the offline "
              @"installer for a jailbroken guest: it shuts down first and adds "
              @"to /Applications without replacing existing apps.\n\n"
              @"If Files cannot provide a selected IPA or IPSW, download it "
              @"in Files first, then choose the local copy." }];

    [e addObject:@{ @"h": @"App Settings",
        @"b": @"App Settings contains jailbreak and IPA installation. Advanced "
              @"contains the emulator interface and firmware import. Firmware import selects the public "
              @"keys automatically for iPhone OS 3.1.3 (7E18), iPhone 3G. "
              @"Manual keys remain available on the Firmware screen.\n\n"
              @"Jailbreak sets up Packages by default. Turn off Advanced → "
              @"Packages with jailbreak to skip it. You can still set it up later; "
              @"this switch does not remove an existing installation.\n\n"
              @"Advanced contains graphics defaults, package-manager setup and "
              @"Reset settings. Turn on Developer Mode there to reveal boot "
              @"options, session defaults and diagnostics. Each boot option "
              @"explains whether it takes effect or is fixed into an existing disk." }];

    [e addObject:@{ @"h": @"Developer mode",
        @"b": @"Turning it on adds: the full sixteen-switch option table, the "
              @"raw guest console, the instruction cap, and diagnostics showing "
              @"what the emulated machine has actually done.\n\nIt changes what "
              @"you can see and set. It does not unlock a faster or more "
              @"complete emulator — there is only one engine." }];

    [e addObject:@{ @"h": @"Graphics status",
        @"b": @"New machines default to experimental MBX graphics, with the "
              @"CPU software-renderer override off. Graphics for new machines "
              @"also offers CPU software as a compatibility option. Choose "
              @"before a machine's first open. Current "
              @"machines then record the exact pair beside their work image and "
              @"reuse it on later starts. Machines created by older builds do "
              @"not: their saved option bits were compile-time defaults, not "
              @"proof of what prepared the image, so they deliberately keep "
              @"the app-wide setting rather than being guessed or migrated.\n\n"
              @"The current MBX model completes the "
              @"measured reset, command-ring, 2D and 3D workloads, including "
              @"a live 1,388/1,388 2D and 8,888/8,888 3D interval with no "
              @"decoder rejection or recovery.\n\nThat is substantial graphics "
              @"coverage. Recent physical runs also survive the historical "
              @"Safari Pages and Spotlight terminal paths, repeated sleep/wake, "
              @"and checkpoint restore. MBX being the default does not imply "
              @"complete rendering coverage, original-device speed, or "
              @"long-session stability on every host." }];

    [e addObject:@{ @"h": @"Guest networking",
        @"b": @"Guest networking and internet routing are on by default for "
              @"new machines. App Settings → Advanced, with Developer Mode on, lets you turn "
              @"them off before the first open. Explicit saved choices are "
              @"preserved. The networking service is installed when the guest "
              @"disk is prepared; changing defaults does not add it to an "
              @"existing offline disk." }];

    if (dev) {
        [e addObject:@{ @"h": @"Console",
            @"b": @"Everything the guest has printed to its serial port, which "
                  @"is where a real iPhone's kernel logs go. On a real firmware "
                  @"boot this is the primary evidence of how far it got.\n\n"
                  @"The built-in test program prints a short banner and then "
                  @"very little, so a nearly-empty console here is correct "
                  @"when no firmware is imported. A firmware boot fills it." }];

        [e addObject:@{ @"h": @"A note on the numbers",
            @"b": @"Instruction counts are the honest unit of progress in this "
                  @"project, not seconds — the same boot takes the same number "
                  @"of instructions every time, and runs are bit-exact "
                  @"reproducible.\n\nWall-clock speed depends on your phone and "
                  @"is not yet optimised: there is no dynamic recompiler in the "
                  @"run loop, so every guest instruction is interpreted." }];
    }

    _entries = e;
    [self.tableView reloadData];
}

#pragma mark - Table

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    (void)tableView;
    return (NSInteger)_entries.count;
}

- (NSInteger)tableView:(UITableView *)tableView
 numberOfRowsInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return 1;
}

- (NSString *)tableView:(UITableView *)tableView
titleForHeaderInSection:(NSInteger)section {
    (void)tableView;
    return _entries[(NSUInteger)section][@"h"];
}

- (UITableViewCell *)tableView:(UITableView *)tableView
         cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    static NSString *const kCell = @"manual";
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:kCell];
    if (!cell)
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                      reuseIdentifier:kCell];
    /* numberOfLines = 0 plus an automatic row height is what makes a paragraph
     * render as a paragraph instead of one clipped line. */
    cell.textLabel.numberOfLines = 0;
    cell.textLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    cell.textLabel.adjustsFontForContentSizeCategory = YES;
    cell.textLabel.text = _entries[(NSUInteger)indexPath.section][@"b"];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    return cell;
}

- (CGFloat)tableView:(UITableView *)tableView
heightForRowAtIndexPath:(NSIndexPath *)indexPath {
    (void)tableView; (void)indexPath;
    return UITableViewAutomaticDimension;
}

- (CGFloat)tableView:(UITableView *)tableView
estimatedHeightForRowAtIndexPath:(NSIndexPath *)indexPath {
    (void)tableView; (void)indexPath;
    return 160.0;
}

@end
