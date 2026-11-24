import AppKit
import Combine

/// A running VM's window: its screen (DisplayView), titled with its name,
/// with a toolbar for power, pause, devices, screenshots, capturing input
/// and full screen. The VM runs in QEMU's process, which shows nothing
/// itself (-display ppcosx), so this window, its menus and its Dock icon
/// are ppcOSX's.
final class VMWindowController: NSWindowController, NSWindowDelegate, NSToolbarDelegate,
                                NSMenuDelegate {
    let vm: VirtualMachine
    let displayView: DisplayView
    private var observers: Set<AnyCancellable> = []
    private var pauseItem: NSToolbarItem?
    private var captureItem: NSToolbarItem?
    private var sizedFor = CGSize.zero
    private var closingConfirmed = false

    init(vm: VirtualMachine, client: DisplayClient) {
        self.vm = vm
        displayView = DisplayView(client: client)
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1024, height: 768),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable],
                              backing: .buffered, defer: false)
        window.collectionBehavior = [.fullScreenPrimary]
        window.contentView = displayView
        window.contentMinSize = NSSize(width: 320, height: 240)
        window.backgroundColor = .black
        window.tabbingMode = .disallowed
        window.isReleasedWhenClosed = false
        window.setFrameAutosaveName("")
        super.init(window: window)
        window.delegate = self

        let toolbar = NSToolbar(identifier: "VMWindow")
        toolbar.delegate = self
        toolbar.displayMode = .iconOnly
        toolbar.allowsUserCustomization = true
        toolbar.autosavesConfiguration = true
        window.toolbar = toolbar
        window.toolbarStyle = .unified

        displayView.onCaptureChange = { [weak self] _ in self?.updateTitle() }
        vm.$config.sink { [weak self] _ in
            DispatchQueue.main.async { self?.updateTitle() }
        }.store(in: &observers)
        vm.$state.sink { [weak self] _ in
            DispatchQueue.main.async { self?.stateChanged() }
        }.store(in: &observers)

        if client.size.width > 0 { guestSizeChanged(client.size) }
        window.center()
        updateTitle()
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    // MARK: title and size

    private func updateTitle() {
        guard let window else { return }
        window.title = vm.config.name
        if displayView.captured {
            window.subtitle = "Press \u{2303}\u{2325} (Control-Option) to release the "
                + (vm.display?.absolute == false ? "pointer" : "keyboard")
        } else {
            switch vm.state {
            case .paused: window.subtitle = "Paused"
            case .starting: window.subtitle = "Starting"
            case .stopping: window.subtitle = "Shutting down"
            default: window.subtitle = ""
            }
        }
        pauseItem?.image = NSImage(systemSymbolName: vm.state == .paused ? "play.fill" : "pause.fill",
                                   accessibilityDescription: nil)
        pauseItem?.label = vm.state == .paused ? "Resume" : "Pause"
        pauseItem?.toolTip = vm.state == .paused ? "Resume the virtual machine"
                                                 : "Pause the virtual machine"
        captureItem?.label = displayView.captured ? "Release Input" : "Capture Input"
    }

    private func stateChanged() {
        updateTitle()
        if vm.state == .stopped {
            closingConfirmed = true
            close()
        }
    }

    /// Fit the window to a new screen size: its size in points, made
    /// smaller to fit the display it is on.
    func guestSizeChanged(_ size: CGSize) {
        guard let window, size.width > 0, size.height > 0, size != sizedFor else { return }
        sizedFor = size
        window.contentAspectRatio = size
        guard !window.styleMask.contains(.fullScreen) else { return }
        fit(size)
    }

    private func fit(_ size: CGSize) {
        guard let window else { return }
        let visible = (window.screen ?? NSScreen.main)?.visibleFrame.size
            ?? CGSize(width: 1440, height: 900)
        let chrome = window.frame.height - window.contentLayoutRect.height
        let s = min(1, (visible.width - 20) / size.width, (visible.height - chrome - 20) / size.height)
        let old = window.frame
        window.setContentSize(NSSize(width: (size.width * s).rounded(),
                                     height: (size.height * s).rounded()))
        // keep the top left corner where it was
        var f = window.frame
        f.origin.y = old.maxY - f.height
        window.setFrame(f, display: true)
    }

    @objc func actualSize(_ sender: Any?) {
        if let size = vm.display?.size, size.width > 0 { fit(size) }
    }

    // MARK: closing

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        guard !closingConfirmed, vm.state.isActive else { return true }
        let alert = NSAlert()
        alert.messageText = "\u{201C}\(vm.config.name)\u{201D} is running"
        alert.informativeText = "Shut it down from here, or keep it running without its window and show it again from the library."
        alert.addButton(withTitle: "Shut Down")
        alert.addButton(withTitle: "Cancel")
        alert.addButton(withTitle: "Keep Running")
        alert.beginSheetModal(for: sender) { [weak self] r in
            guard let self else { return }
            switch r {
            case .alertFirstButtonReturn:
                self.vm.shutDown()
            case .alertThirdButtonReturn:
                self.closingConfirmed = true
                self.close()
            default:
                break
            }
        }
        return false
    }

    func windowWillClose(_ notification: Notification) {
        displayView.release()
        closingConfirmed = false
        vm.windowClosed()
    }

    func windowDidBecomeKey(_ notification: Notification) {
        window?.makeFirstResponder(displayView)
        vm.windowBecameKey()
    }

    // MARK: toolbar

    private enum Item {
        static let power = NSToolbarItem.Identifier("power")
        static let pause = NSToolbarItem.Identifier("pause")
        static let devices = NSToolbarItem.Identifier("devices")
        static let screenshot = NSToolbarItem.Identifier("screenshot")
        static let capture = NSToolbarItem.Identifier("capture")
        static let fullScreen = NSToolbarItem.Identifier("fullscreen")
    }

    func toolbarDefaultItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        [.flexibleSpace, Item.power, Item.pause, Item.devices, Item.screenshot,
         Item.capture, Item.fullScreen]
    }

    func toolbarAllowedItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        [Item.power, Item.pause, Item.devices, Item.screenshot, Item.capture, Item.fullScreen,
         .flexibleSpace, .space]
    }

    func toolbar(_ toolbar: NSToolbar, itemForItemIdentifier id: NSToolbarItem.Identifier,
                 willBeInsertedIntoToolbar flag: Bool) -> NSToolbarItem? {
        func symbol(_ name: String, _ label: String) -> NSImage? {
            NSImage(systemSymbolName: name, accessibilityDescription: label)
        }
        switch id {
        case Item.power:
            let item = NSMenuToolbarItem(itemIdentifier: id)
            let menu = NSMenu()
            menu.addItem(withTitle: "Shut Down", action: #selector(shutDown(_:)), keyEquivalent: "")
            menu.addItem(withTitle: "Restart\u{2026}", action: #selector(restart(_:)), keyEquivalent: "")
            menu.addItem(.separator())
            menu.addItem(withTitle: "Force Power Off\u{2026}", action: #selector(forcePowerOff(_:)),
                         keyEquivalent: "")
            menu.items.forEach { $0.target = self }
            item.menu = menu
            item.image = symbol("power", "Power")
            item.label = "Power"
            item.toolTip = "Shut down, restart or power off the virtual machine"
            item.showsIndicator = false
            return item
        case Item.pause:
            let item = NSToolbarItem(itemIdentifier: id)
            item.target = self
            item.action = #selector(togglePause(_:))
            item.isBordered = true
            pauseItem = item
            updateTitle()
            return item
        case Item.devices:
            let item = NSMenuToolbarItem(itemIdentifier: id)
            let menu = NSMenu()
            menu.delegate = self
            item.menu = menu
            item.image = symbol("opticaldiscdrive", "Devices")
            item.label = "Devices"
            item.toolTip = "Discs, USB devices and network of the virtual machine"
            item.showsIndicator = false
            return item
        case Item.screenshot:
            let item = NSToolbarItem(itemIdentifier: id)
            item.image = symbol("camera", "Screenshot")
            item.label = "Screenshot"
            item.toolTip = "Save a picture of the screen to the Desktop"
            item.target = self
            item.action = #selector(screenshot(_:))
            item.isBordered = true
            return item
        case Item.capture:
            let item = NSToolbarItem(itemIdentifier: id)
            item.image = symbol("cursorarrow.rays", "Capture Input")
            item.label = "Capture Input"
            item.toolTip = "Send the keyboard (Command shortcuts too) and pointer to the virtual machine; Control-Option releases them"
            item.target = self
            item.action = #selector(toggleCapture(_:))
            item.isBordered = true
            captureItem = item
            return item
        case Item.fullScreen:
            let item = NSToolbarItem(itemIdentifier: id)
            item.image = symbol("arrow.up.left.and.arrow.down.right", "Full Screen")
            item.label = "Full Screen"
            item.target = window
            item.action = #selector(NSWindow.toggleFullScreen(_:))
            item.isBordered = true
            return item
        default:
            return nil
        }
    }

    // MARK: actions

    @objc func shutDown(_ sender: Any?) { vm.shutDown() }

    @objc func restart(_ sender: Any?) {
        confirm("Restart \u{201C}\(vm.config.name)\u{201D}?",
                "It restarts at once, as with a reset button: anything unsaved in it is lost.",
                "Restart") { $0.restart() }
    }

    @objc func forcePowerOff(_ sender: Any?) {
        confirm("Power off \u{201C}\(vm.config.name)\u{201D}?",
                "It stops at once: anything unsaved in it is lost.",
                "Force Power Off") { $0.forceStop() }
    }

    private func confirm(_ message: String, _ info: String, _ button: String,
                         _ action: @escaping (VirtualMachine) -> Void) {
        guard let window else { return }
        let alert = NSAlert()
        alert.messageText = message
        alert.informativeText = info
        alert.alertStyle = .warning
        alert.addButton(withTitle: button)
        alert.addButton(withTitle: "Cancel")
        alert.beginSheetModal(for: window) { [vm] r in
            if r == .alertFirstButtonReturn { action(vm) }
        }
    }

    @objc func togglePause(_ sender: Any?) {
        if vm.state == .paused { vm.resume() } else { vm.pause() }
    }

    @objc func screenshot(_ sender: Any?) { vm.screenshot() }

    @objc func toggleCapture(_ sender: Any?) {
        if displayView.captured { displayView.release() } else { displayView.capture() }
    }

    // MARK: devices menu

    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        DevicesMenu.build(menu, for: vm)
    }
}

/// The devices of a running VM as menu items: the toolbar's Devices menu and
/// the menu bar's Virtual Machine > Devices.
@MainActor
enum DevicesMenu {
    static func build(_ menu: NSMenu, for vm: VirtualMachine) {
        menu.autoenablesItems = false
        vm.refreshDevices()
        let live = vm.devices

        menu.addItem(.sectionHeader(title: "Storage"))
        for drive in vm.config.drives where drive.type == .cdrom {
            let file = live.media[drive.id] ?? nil
            let item = row(menu, "CD/DVD Drive",
                           file.map { ($0 as NSString).lastPathComponent } ?? "No disc",
                           "opticaldisc")
            let sub = NSMenu()
            sub.addItem(action("Choose Disc Image\u{2026}") { vm.chooseDisc(drive.id) })
            let eject = action("Eject") { vm.eject(drive.id) }
            eject.isEnabled = file != nil
            sub.addItem(eject)
            item.submenu = sub
        }

        menu.addItem(.sectionHeader(title: "USB"))
        let host = HostUSB.devices()
        for d in host {
            let attached = live.usb.contains(d.qdevID)
            let item = action(d.name) { vm.setUSB(d, attached: !attached) }
            item.image = NSImage(systemSymbolName: "cable.connector", accessibilityDescription: nil)
            item.state = attached ? .on : .off
            item.toolTip = attached ? "Connected to the virtual machine: choose to give it back to this Mac"
                                    : "Choose to connect it to the virtual machine"
            menu.addItem(item)
        }
        if host.isEmpty {
            row(menu, "USB", "No devices", "cable.connector").isEnabled = false
        }

        menu.addItem(.sectionHeader(title: "Network"))
        if vm.config.network.mode == .nat {
            let item = row(menu, "Ethernet", live.cableConnected ? "Connected (shared with this Mac)"
                                                                 : "Cable disconnected", "network")
            let sub = NSMenu()
            sub.addItem(action(live.cableConnected ? "Disconnect Cable" : "Connect Cable") {
                vm.setCable(connected: !live.cableConnected)
            })
            item.submenu = sub
        } else {
            row(menu, "Ethernet", "None", "network.slash").isEnabled = false
        }

        menu.addItem(.sectionHeader(title: "Sound"))
        row(menu, "Sound", vm.config.audio.enabled ? "On" : "Off",
            vm.config.audio.enabled ? "speaker.wave.2" : "speaker.slash").isEnabled = false
    }

    @discardableResult
    private static func row(_ menu: NSMenu, _ title: String, _ detail: String,
                            _ symbol: String) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        item.image = NSImage(systemSymbolName: symbol, accessibilityDescription: title)
        if #available(macOS 14.4, *) {
            item.subtitle = detail
        } else {
            item.title = "\(title) \u{2014} \(detail)"
        }
        menu.addItem(item)
        return item
    }

    private static func action(_ title: String, _ run: @escaping @MainActor () -> Void) -> NSMenuItem {
        let item = ClosureMenuItem(title: title, run: run)
        return item
    }
}

/// A menu item that runs a closure.
final class ClosureMenuItem: NSMenuItem {
    private let run: @MainActor () -> Void

    init(title: String, run: @escaping @MainActor () -> Void) {
        self.run = run
        super.init(title: title, action: #selector(fire(_:)), keyEquivalent: "")
        target = self
    }

    required init(coder: NSCoder) { fatalError("not used") }

    @objc private func fire(_ sender: Any?) { MainActor.assumeIsolated { run() } }
}
