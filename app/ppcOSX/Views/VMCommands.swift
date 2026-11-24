import SwiftUI

/// The menu bar's Virtual Machine menu: what a VM window's toolbar does, for
/// the VM in front (or the one selected in the library).
struct VMCommands: Commands {
    @ObservedObject var focus = VMFocus.shared

    var body: some Commands {
        CommandMenu("Virtual Machine") {
            if let vm = focus.target {
                VMMenuItems(vm: vm)
            } else {
                Button("Start") {}.disabled(true)
            }
        }
        CommandGroup(after: .toolbar) {
            Button("Actual Size") {
                (NSApp.keyWindow?.windowController as? VMWindowController)?.actualSize(nil)
            }
            .keyboardShortcut("0", modifiers: [.command, .option])
            .disabled(focus.machine == nil)
            Divider()
        }
    }
}

private struct VMMenuItems: View {
    @ObservedObject var vm: VirtualMachine

    var body: some View {
        if vm.state.isActive {
            Button("Show Window") { vm.showWindow() }
            Divider()
            if vm.state == .paused {
                Button("Resume") { vm.resume() }
                    .keyboardShortcut("p", modifiers: [.command, .option])
            } else {
                Button("Pause") { vm.pause() }
                    .keyboardShortcut("p", modifiers: [.command, .option])
                    .disabled(vm.state != .running)
            }
            Button("Shut Down") { vm.shutDown() }
            Button("Restart\u{2026}") { confirm(vm, restart: true) }
            Button("Force Power Off\u{2026}") { confirm(vm, restart: false) }
            Divider()
            Button("Capture Keyboard and Pointer") { vm.toggleCapture() }
                .keyboardShortcut("c", modifiers: [.command, .option])
                .disabled(!vm.hasWindow)
            Button("Take Screenshot") { vm.screenshot() }
                .keyboardShortcut("3", modifiers: [.command, .option])
            Divider()
            Menu("Devices") { DevicesMenuView(vm: vm) }
        } else {
            Button("Start \u{201C}\(vm.config.name)\u{201D}") { vm.start() }
        }
    }

    private func confirm(_ vm: VirtualMachine, restart: Bool) {
        let alert = NSAlert()
        alert.messageText = restart ? "Restart \u{201C}\(vm.config.name)\u{201D}?"
                                    : "Power off \u{201C}\(vm.config.name)\u{201D}?"
        alert.informativeText = restart
            ? "It restarts at once, as with a reset button: anything unsaved in it is lost."
            : "It stops at once: anything unsaved in it is lost."
        alert.alertStyle = .warning
        alert.addButton(withTitle: restart ? "Restart" : "Force Power Off")
        alert.addButton(withTitle: "Cancel")
        if alert.runModal() == .alertFirstButtonReturn {
            if restart { vm.restart() } else { vm.forceStop() }
        }
    }
}

/// Virtual Machine > Devices: discs, USB devices and the network cable.
private struct DevicesMenuView: View {
    @ObservedObject var vm: VirtualMachine

    var body: some View {
        let cds = vm.config.drives.filter { $0.type == .cdrom }
        ForEach(cds) { drive in
            Menu("CD/DVD Drive") {
                Button("Choose Disc Image\u{2026}") { vm.chooseDisc(drive.id) }
                Button("Eject") { vm.eject(drive.id) }
            }
        }
        Divider()
        Menu("USB") {
            let host = HostUSB.devices()
            if host.isEmpty {
                Text("No USB Devices")
            }
            ForEach(host) { d in
                let attached = vm.devices.usb.contains(d.qdevID)
                Toggle(d.name, isOn: Binding(get: { attached },
                                             set: { vm.setUSB(d, attached: $0) }))
            }
        }
        .onAppear { vm.refreshDevices() }
        if vm.config.network.mode == .nat {
            Toggle("Ethernet Cable Connected",
                   isOn: Binding(get: { vm.devices.cableConnected },
                                 set: { vm.setCable(connected: $0) }))
        }
    }
}
