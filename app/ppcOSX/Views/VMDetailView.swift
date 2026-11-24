import SwiftUI

struct VMDetailView: View {
    @ObservedObject var vm: VirtualMachine
    var edit: () -> Void
    var delete: () -> Void
    @State private var notes = ""

    var body: some View {
        Form {
            if let error = vm.lastError {
                Section {
                    Label {
                        Text(error).textSelection(.enabled)
                    } icon: {
                        Image(systemName: "exclamationmark.triangle.fill")
                            .foregroundStyle(.orange)
                    }
                    .font(.callout)
                }
            }

            Section("System") {
                LabeledContent("Operating system", value: vm.config.guest.title)
                LabeledContent("Processor", value: vm.config.cpu.title)
                LabeledContent("Cores", value: "\(vm.config.cores)")
                LabeledContent("Memory", value: MemoryLimits.format(vm.config.memory))
                LabeledContent("Graphics", value: vm.config.graphics.title)
                LabeledContent("Acceleration",
                               value: vm.config.graphics.features(for: vm.config.guest))
            }

            Section("Storage") {
                ForEach(vm.config.drives) { drive in
                    DriveRow(drive: drive, folder: vm.folder,
                             compact: vm.canCompact(drive) ? { vm.compact(drive) } : nil,
                             compacting: vm.compacting == drive.id)
                }
                LabeledContent("Starts from", value: vm.config.boot.title)
                LabeledContent("Space used",
                               value: ByteCountFormatter.string(fromByteCount: vm.diskUsage,
                                                                countStyle: .file))
            }

            Section("Devices") {
                LabeledContent("Network", value: vm.config.network.mode == .nat
                    ? "Shared (NAT)" + (vm.config.network.forwards.isEmpty ? ""
                        : ", " + vm.config.network.forwards.map { "\($0.host)\u{2192}\($0.guest)" }
                            .joined(separator: ", "))
                    : "Not connected")
                LabeledContent("Keyboard and mouse", value: vm.config.input.title)
                LabeledContent("USB devices", value: vm.config.usb.isEmpty ? "None"
                               : vm.config.usb.map(\.name).joined(separator: ", "))
                LabeledContent("Screen sharing") {
                    HStack {
                        Text(vm.config.sharing.enabled
                             ? "VNC on port \(vm.config.sharing.port)"
                               + (vm.config.sharing.tls ? ", TLS" : "")
                             : "Off")
                        if vm.config.sharing.enabled && vm.state.isActive && !vm.config.sharing.tls {
                            Button("Open") {
                                NSWorkspace.shared.open(URL(string: "vnc://127.0.0.1:\(vm.config.sharing.port)")!)
                            }
                            .help("Open the shared screen in Screen Sharing")
                        }
                    }
                }
                LabeledContent("Sound", value: vm.config.audio.enabled ? "On" : "Off")
            }

            Section("Notes") {
                NotesEditor(text: $notes)
                    .onAppear { notes = vm.config.notes }
                    .onDisappear { saveNotes() }
                    .onChange(of: notes) { _, _ in saveNotes() }
            }

        }
        .formStyle(.grouped)
        .navigationTitle(vm.config.name)
        .toolbar {
            ToolbarItemGroup {
                if vm.state.isActive {
                    Button { vm.showWindow() } label: {
                        Label("Show Window", systemImage: "macwindow")
                    }
                    .help("Show the virtual machine's window")
                    Button { vm.screenshot() } label: {
                        Label("Screenshot", systemImage: "camera")
                    }
                    .help("Save a picture of the screen to the Desktop")
                    if vm.state == .paused {
                        Button { vm.resume() } label: { Label("Resume", systemImage: "play.fill") }
                    } else {
                        Button { vm.pause() } label: { Label("Pause", systemImage: "pause.fill") }
                            .disabled(vm.state != .running)
                    }
                    Menu {
                        Button("Shut Down") { vm.shutDown() }
                        Button("Restart") { vm.restart() }
                        Divider()
                        Button("Force Power Off") { vm.forceStop() }
                    } label: {
                        Label("Power", systemImage: "power")
                    }
                } else {
                    Button { vm.start() } label: { Label("Start", systemImage: "play.fill") }
                        .help("Start the virtual machine")
                }
                Button(action: edit) { Label("Settings", systemImage: "slider.horizontal.3") }
                    .help("Change the virtual machine's settings")
            }
        }
    }

    private func saveNotes() {
        if notes != vm.config.notes { vm.setNotes(notes) }
    }
}

struct DriveRow: View {
    let drive: Drive
    let folder: URL
    /// shrink the image (local qcow2 disks, VM stopped)
    var compact: (() -> Void)?
    var compacting = false

    var body: some View {
        let url = drive.url(in: folder)
        LabeledContent {
            HStack {
                if drive.isRemote {
                    Image(systemName: "network").foregroundStyle(.secondary)
                }
                Text(drive.displayName(in: folder))
                    .foregroundStyle(url == nil ? .secondary : .primary)
                    .lineLimit(1).truncationMode(.middle)
                if let url, !drive.isRemote, !FileManager.default.fileExists(atPath: url.path) {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                        .help("This file is missing")
                }
                if compacting {
                    ProgressView().controlSize(.small)
                } else if let compact {
                    Button("Compact", action: compact)
                        .help("Rewrite the disk image compressed (zstd), to take less space")
                }
            }
        } label: {
            Label(Self.title(drive.type), systemImage: Self.symbol(drive.type))
        }
    }

    static func title(_ kind: Drive.Kind) -> String {
        switch kind {
        case .disk: return "Hard Disk"
        case .cdrom: return "CD/DVD Drive"
        case .usb: return "USB Drive"
        }
    }

    static func symbol(_ kind: Drive.Kind) -> String {
        switch kind {
        case .disk: return "internaldrive"
        case .cdrom: return "opticaldisc"
        case .usb: return "externaldrive"
        }
    }
}

/// A text editor that blends into its form row: no background of its own,
/// so the row shows no darker box inside it.
struct NotesEditor: View {
    @Binding var text: String

    var body: some View {
        TextEditor(text: $text)
            .font(.body)
            .scrollContentBackground(.hidden)
            .frame(minHeight: 72)
    }
}
