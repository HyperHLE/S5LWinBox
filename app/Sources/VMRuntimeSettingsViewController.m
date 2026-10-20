// See VMRuntimeSettingsViewController.h.
// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMRuntimeSettingsViewController.h"
#import "VMSettings.h" // Read-only cap choices, not the shared preferences.
#import "VMIPALibraryViewController.h"
#import "VMIPAInstallViewController.h"
#import "VMPackageManagerViewController.h"
#import "VMGuestFilesViewController.h"

typedef NS_ENUM(NSInteger, VMRuntimeSection) {
    VMRuntimeSectionSession,
    VMRuntimeSectionApps,
    VMRuntimeSectionSnapshots,
    VMRuntimeSectionPower,
    VMRuntimeSectionAdvanced,
    VMRuntimeSectionDeveloper,
};

@interface VMRuntimeSettingsViewController ()
@property (nonatomic) BOOL advanced;
- (void)refreshRuntimeState;
@end

@implementation VMRuntimeSettingsViewController {
    NSTimer *_stateTimer;
    NSArray *_lastRuntimeState;
}

- (instancetype)init {
    return [self initWithStyle:UITableViewStyleInsetGrouped];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = self.advanced ? @"Advanced" : @"Machine Settings";
    self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
    self.tableView.accessibilityIdentifier = self.advanced
        ? @"s5lbox.machine-settings.advanced" : @"s5lbox.machine-settings";
    self.tableView.rowHeight = UITableViewAutomaticDimension;
    self.tableView.estimatedRowHeight = 65;
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc]
        initWithBarButtonSystemItem:UIBarButtonSystemItemDone
        target:self action:@selector(doneTapped)];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    _lastRuntimeState = nil;
    [self refreshRuntimeState];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [_stateTimer invalidate];
    __weak VMRuntimeSettingsViewController *weakSelf = self;
    _stateTimer = [NSTimer scheduledTimerWithTimeInterval:0.25 repeats:YES block:^(__unused NSTimer *timer) {
        [weakSelf refreshRuntimeState];
    }];
    _stateTimer.tolerance = 0.05;
}

- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    [_stateTimer invalidate];
    _stateTimer = nil;
}

- (void)dealloc { [_stateTimer invalidate]; }

- (void)refreshRuntimeState {
    id<VMRuntimeSettingsDelegate> owner = self.runtimeDelegate;
    NSArray *state = @[@(owner != nil), @(self.usbTransport != nil),
        @([owner runtimeCanControlGuest]), @([owner runtimeCanShutDown]),
        @([owner runtimeCanForcePowerOff]), @([owner runtimePaused]),
        @([owner runtimeMicrophoneEnabled]), [owner runtimeAudioStatus] ?: @"",
        @([owner runtimePausesInBackground]), @([owner runtimeInlineConsole]),
        @([owner runtimeInstructionCap])];
    // Boot/restore can finish while this sheet is open. Refresh only actual
    // state changes, not every tick (which would disrupt scrolling/VoiceOver).
    if ([_lastRuntimeState isEqual:state]) return;
    _lastRuntimeState = state;
    [self.tableView reloadData];
}

- (void)doneTapped {
    [self dismissViewControllerAnimated:YES completion:nil];
}

- (VMRuntimeSection)sectionAt:(NSInteger)section {
    if (self.advanced)
        return section == 0 ? VMRuntimeSectionSession : VMRuntimeSectionDeveloper;
    const VMRuntimeSection sections[] = { VMRuntimeSectionApps, VMRuntimeSectionSnapshots,
        VMRuntimeSectionPower, VMRuntimeSectionAdvanced };
    return sections[section];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    (void)tableView;
    return self.advanced ? (self.showsDeveloperControls ? 2 : 1) : 4;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void)tableView;
    switch ([self sectionAt:section]) {
        case VMRuntimeSectionSession: return 2;
        case VMRuntimeSectionApps: return 3;
        case VMRuntimeSectionSnapshots: return 1;
        case VMRuntimeSectionPower: return 4;
        case VMRuntimeSectionAdvanced: return 1;
        case VMRuntimeSectionDeveloper: return self.showsDeveloperControls ? 2 : 0;
    }
    return 0;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    (void)tableView;
    switch ([self sectionAt:section]) {
        case VMRuntimeSectionSession: return self.machineName.length ? self.machineName : @"Current machine";
        case VMRuntimeSectionApps: return @"Guest apps";
        case VMRuntimeSectionSnapshots: return @"Saved states";
        case VMRuntimeSectionPower: return @"Power";
        case VMRuntimeSectionAdvanced: return nil;
        case VMRuntimeSectionDeveloper: return @"Session diagnostics";
    }
    return nil;
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void)tableView;
    section = [self sectionAt:section];
    if (section == VMRuntimeSectionSession)
        return @"Changes apply to this session only.";
    if (section == VMRuntimeSectionPower)
        return @"Save & close keeps your place. Shut down powers off iPhone OS.";
    if (section == VMRuntimeSectionDeveloper)
        return nil;
    return nil;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)path {
    VMRuntimeSection section = [self sectionAt:path.section];
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"runtime"];
    if (!cell) cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                           reuseIdentifier:@"runtime"];
    cell.accessoryView = nil;
    cell.accessoryType = UITableViewCellAccessoryNone;
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.textLabel.text = nil;
    cell.detailTextLabel.text = nil;
    cell.textLabel.textColor = [UIColor labelColor];
    cell.detailTextLabel.textColor = [UIColor secondaryLabelColor];
    cell.textLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    cell.detailTextLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1];
    cell.textLabel.numberOfLines = cell.detailTextLabel.numberOfLines = 0;
    cell.textLabel.adjustsFontForContentSizeCategory = YES;
    cell.detailTextLabel.adjustsFontForContentSizeCategory = YES;
    cell.accessibilityIdentifier = nil;

    id<VMRuntimeSettingsDelegate> owner = self.runtimeDelegate;
    if (section == VMRuntimeSectionSession ||
        (section == VMRuntimeSectionDeveloper && path.row == 0)) {
        UISwitch *toggle = [[UISwitch alloc] init];
        if (section == VMRuntimeSectionDeveloper) {
            cell.textLabel.text = @"Console under the screen";
            cell.detailTextLabel.text = @"Show live guest output beneath the display.";
            toggle.on = [owner runtimeInlineConsole];
            toggle.accessibilityIdentifier = @"s5lbox.machine-settings.console";
            [toggle addTarget:self action:@selector(consoleChanged:)
                      forControlEvents:UIControlEventValueChanged];
        } else if (path.row == 0) {
            cell.textLabel.text = @"Pause in background";
            cell.detailTextLabel.text = @"Recommended. iOS may terminate the app if disabled.";
            toggle.on = [owner runtimePausesInBackground];
            toggle.accessibilityIdentifier = @"s5lbox.machine-settings.background";
            [toggle addTarget:self action:@selector(backgroundChanged:)
                      forControlEvents:UIControlEventValueChanged];
        } else {
            cell.textLabel.text = @"Guest microphone";
            cell.detailTextLabel.text = [owner runtimeAudioStatus];
            toggle.on = [owner runtimeMicrophoneEnabled];
            toggle.enabled = [owner runtimeCanControlGuest];
            toggle.accessibilityIdentifier = @"s5lbox.machine-settings.microphone";
            [toggle addTarget:self action:@selector(microphoneChanged:)
                      forControlEvents:UIControlEventValueChanged];
        }
        toggle.accessibilityLabel = cell.textLabel.text;
        cell.accessoryView = toggle;
        return cell;
    }
    BOOL enabled = owner != nil;
    if (section == VMRuntimeSectionApps) {
        cell.textLabel.text = path.row == 0 ? @"Install IPA" : path.row == 1 ? @"Packages" : @"Files";
        enabled = self.usbTransport && [owner runtimeCanControlGuest] && ![owner runtimePaused];
        // Explain an unavailable action, rather than repeat its label when it works.
        cell.detailTextLabel.text = [owner runtimePaused] ? @"Resume the guest first."
            : ![owner runtimeCanControlGuest] ? @"Start the guest first."
            : !self.usbTransport ? @"Guest connection unavailable." : nil;
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        cell.accessibilityIdentifier = @"s5lbox.machine-settings.ipa";
        if (path.row == 1) {
            cell.accessibilityIdentifier = @"s5lbox.machine-settings.packages";
        }
        if (path.row == 2) cell.accessibilityIdentifier = @"s5lbox.machine-settings.files";
    } else if (section == VMRuntimeSectionSnapshots) {
        cell.textLabel.text = @"Snapshots";
        enabled = enabled && self.snapshotsDirectory.length > 0;
        cell.accessoryType = enabled ? UITableViewCellAccessoryDisclosureIndicator : UITableViewCellAccessoryNone;
        cell.accessibilityIdentifier = @"s5lbox.machine-settings.snapshots";
    } else if (section == VMRuntimeSectionPower) {
        if (path.row == VMRuntimeActionSaveAndClose) {
            cell.textLabel.text = [owner runtimeCanShutDown] ? @"Save & close" : @"Close machine";
            cell.accessibilityIdentifier = @"s5lbox.machine-settings.close";
        } else if (path.row == VMRuntimeActionShutDown) {
            cell.textLabel.text = @"Shut down";
            enabled = [owner runtimeCanShutDown];
            cell.accessibilityIdentifier = @"s5lbox.machine-settings.shutdown";
        } else if (path.row == VMRuntimeActionRestart) {
            cell.textLabel.text = @"Restart guest";
            cell.detailTextLabel.text = @"Unsaved work may be lost.";
            cell.textLabel.textColor = [UIColor systemRedColor];
            cell.accessibilityIdentifier = @"s5lbox.machine-settings.restart";
        } else {
            cell.textLabel.text = @"Force Power Off";
            cell.detailTextLabel.text = @"Works during boot. Unsaved work may be lost.";
            enabled = [owner runtimeCanForcePowerOff];
            cell.textLabel.textColor = [UIColor systemRedColor];
            cell.accessibilityIdentifier = @"s5lbox.machine-settings.force-power-off";
        }
    } else if (section == VMRuntimeSectionAdvanced) {
        cell.textLabel.text = @"Advanced";
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        cell.accessibilityIdentifier = @"s5lbox.machine-settings.open-advanced";
    } else {
        uint64_t cap = [owner runtimeInstructionCap];
        cell.textLabel.text = @"Instruction limit";
        cell.detailTextLabel.text = cap ? [NSString stringWithFormat:@"%llu total instructions", (unsigned long long)cap] : @"No limit";
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        cell.accessibilityIdentifier = @"s5lbox.machine-settings.limit";
    }
    cell.selectionStyle = enabled ? UITableViewCellSelectionStyleDefault : UITableViewCellSelectionStyleNone;
    if (!enabled) cell.textLabel.textColor = [UIColor secondaryLabelColor];
    return cell;
}

- (void)backgroundChanged:(UISwitch *)toggle {
    [self.runtimeDelegate setRuntimePausesInBackground:toggle.on];
}
- (void)microphoneChanged:(UISwitch *)toggle {
    toggle.enabled = NO;
    __weak VMRuntimeSettingsViewController *weakSelf = self;
    [self.runtimeDelegate setRuntimeMicrophoneEnabled:toggle.on completion:^(BOOL okay, NSString *message) {
        VMRuntimeSettingsViewController *self = weakSelf;
        if (!self) return;
        [self.tableView reloadData];
        if (!okay && self.view.window) {
            UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Microphone is off"
                message:message preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
            [self presentViewController:alert animated:YES completion:nil];
        }
    }];
}
- (void)consoleChanged:(UISwitch *)toggle {
    [self.runtimeDelegate setRuntimeInlineConsole:toggle.on];
}

- (void)performAfterDismiss:(VMRuntimeAction)action {
    id<VMRuntimeSettingsDelegate> owner = self.runtimeDelegate;
    /* Dismiss the whole settings sheet, including any confirmation alert,
     * before the owner can present failure UI or navigate back to Machines. */
    UIViewController *presenter = self.navigationController.presentingViewController;
    if (!presenter) return;
    [presenter dismissViewControllerAnimated:YES completion:^{
        [owner performRuntimeAction:action];
    }];
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)path {
    [tableView deselectRowAtIndexPath:path animated:YES];
    VMRuntimeSection section = [self sectionAt:path.section];
    if (section == VMRuntimeSectionAdvanced) {
        VMRuntimeSettingsViewController *advanced = [[VMRuntimeSettingsViewController alloc] init];
        advanced.advanced = YES;
        advanced.runtimeDelegate = self.runtimeDelegate;
        advanced.machineName = self.machineName;
        advanced.showsDeveloperControls = self.showsDeveloperControls;
        [self.navigationController pushViewController:advanced animated:YES];
    } else if (section == VMRuntimeSectionApps) {
        if (!self.usbTransport || ![self.runtimeDelegate runtimeCanControlGuest] || [self.runtimeDelegate runtimePaused]) return;
        if (path.row == 2) {
            VMGuestFilesViewController *files = [[VMGuestFilesViewController alloc] initWithTransport:self.usbTransport instanceID:self.instanceID];
            [self.navigationController pushViewController:files animated:YES]; return;
        }
        if (path.row == 1) {
            id<VMRuntimeSettingsDelegate> owner = self.runtimeDelegate;
            UIViewController *presenter = self.navigationController.presentingViewController;
            [presenter dismissViewControllerAnimated:YES completion:^{ [owner openRuntimePackages]; }];
            return;
        }
        VMIPALibraryViewController *library = [[VMIPALibraryViewController alloc] init];
        __weak VMRuntimeSettingsViewController *weakSelf = self;
        __weak VMIPALibraryViewController *weakLibrary = library;
        library.selectionHandler = ^(NSURL *url) {
            VMRuntimeSettingsViewController *owner = weakSelf;
            VMIPALibraryViewController *collection = weakLibrary;
            if (!owner || !collection || collection.navigationController.topViewController != collection) return;
            VMIPAInstallViewController *install = [[VMIPAInstallViewController alloc]
                initWithInstanceID:owner.instanceID machineName:owner.machineName];
            install.liveTransport = owner.usbTransport;
            install.readyHandler = ^{ [weakSelf dismissViewControllerAnimated:YES completion:nil]; };
            [collection.navigationController pushViewController:install animated:YES];
            [install inspectURL:url];
        };
        [self.navigationController pushViewController:library animated:YES];
    } else if (section == VMRuntimeSectionSnapshots) {
        if (!self.snapshotsDirectory.length || !self.runtimeDelegate) return;
        VMSnapshotListViewController *list = [[VMSnapshotListViewController alloc] init];
        list.snapshotsDirectory = self.snapshotsDirectory;
        list.delegate = self.runtimeDelegate;
        [self.navigationController pushViewController:list animated:YES];
    } else if (section == VMRuntimeSectionPower) {
        VMRuntimeAction action = (VMRuntimeAction)path.row;
        if (!self.runtimeDelegate || (action == VMRuntimeActionShutDown &&
            ![self.runtimeDelegate runtimeCanShutDown])) return;
        if (action == VMRuntimeActionForcePowerOff &&
            ![self.runtimeDelegate runtimeCanForcePowerOff]) return;
        if (action != VMRuntimeActionRestart) {
            [self performAfterDismiss:action];
            return;
        }
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Restart this guest?"
            message:@"Shut down iPhone OS and start it again."
            preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
        __weak VMRuntimeSettingsViewController *weakSelf = self;
        [alert addAction:[UIAlertAction actionWithTitle:@"Restart" style:UIAlertActionStyleDestructive
            handler:^(__unused UIAlertAction *a) { [weakSelf performAfterDismiss:VMRuntimeActionRestart]; }]];
        [self presentViewController:alert animated:YES completion:nil];
    } else if (section == VMRuntimeSectionDeveloper && path.row == 1) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Instruction limit"
            message:@"Total instructions since boot. A limit already passed stops the guest immediately; it cannot resume that CPU state."
            preferredStyle:UIAlertControllerStyleActionSheet];
        __weak VMRuntimeSettingsViewController *weakSelf = self;
        for (NSNumber *choice in [VMSettings instructionCapChoices]) {
            uint64_t cap = choice.unsignedLongLongValue;
            NSString *label = !cap ? @"No limit" : cap >= 1000000000ull
                ? [NSString stringWithFormat:@"%llu billion", (unsigned long long)(cap / 1000000000ull)]
                : [NSString stringWithFormat:@"%llu million", (unsigned long long)(cap / 1000000ull)];
            [alert addAction:[UIAlertAction actionWithTitle:label
                style:cap ? UIAlertActionStyleDestructive : UIAlertActionStyleDefault
                handler:^(__unused UIAlertAction *a) {
                    [weakSelf.runtimeDelegate setRuntimeInstructionCap:cap];
                    [weakSelf.tableView reloadData];
                }]];
        }
        [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
        alert.popoverPresentationController.sourceView = tableView;
        alert.popoverPresentationController.sourceRect = [tableView rectForRowAtIndexPath:path];
        [self presentViewController:alert animated:YES completion:nil];
    }
}
@end
