import SwiftUI

/// A VM's settings, edited on a copy and saved together.
struct VMSettingsView: View {
    @ObservedObject var vm: VirtualMachine
    @Environment(\.dismiss) private var dismiss
    @State private var config: VMConfig
    /// new disk images to make on saving: drive id -> GB
    @State private var newDisks: [String: Int] = [:]
    @State private var saving = false
    @State private var error: String?
    @State private var tab = "general"
    @State private var vncPassword = ""

    init(vm: VirtualMachine) {
        self.vm = vm
        _config = State(initialValue: vm.config)
        _vncPassword = State(initialValue: Secrets.vncPassword(for: vm.config.uuid) ?? "")
    }

    private var running: Bool { vm.state.isActive }

    var body: some View {
        VStack(spacing: 0) {
            TabView(selection: $tab) {
                Form {
                    Section {
                        TextField("Name", text: $config.name)
                        Picker("System", selection: $config.guest) {
                            ForEach(GuestOS.allCases) { Text($0.title).tag($0) }
                        }
                        .disabled(running)
                    }
                    Section("Notes") {
                        NotesEditor(text: $config.notes)
                    }
                    Section("Location") {
                        LabeledContent("Folder") {
                            HStack {
                                Text(vm.folder.path).lineLimit(1).truncationMode(.middle)
                                    .textSelection(.enabled)
                                Button("Show") {
                                    NSWorkspace.shared.activateFileViewerSelecting([vm.folder])
                                }
                            }
                        }
                    }
                }
                .settingsPage()
                .tabItem { Label("General", systemImage: "gearshape") }
                .tag("general")

                Form { SystemSection(config: $config) }
                    .settingsPage()
                    .disabled(running)
                    .tabItem { Label("System", systemImage: "cpu") }
                    .tag("system")

                Form { DisplaySection(config: $config) }
                    .settingsPage()
                    .disabled(running)
                    .tabItem { Label("Display", systemImage: "display") }
                    .tag("display")

                StorageEditor(config: $config, newDisks: $newDisks, folder: vm.folder)
                    .settingsPage()
                    .disabled(running)
                    .tabItem { Label("Storage", systemImage: "internaldrive") }
                    .tag("storage")

                Form {
                    NetworkSection(network: $config.network)
                    SoundSection(audio: $config.audio)
                }
                .settingsPage()
                .disabled(running)
                .tabItem { Label("Network & Sound", systemImage: "network") }
                .tag("network")

                Form { USBSection(usb: $config.usb) }
                    .settingsPage()
                    .disabled(running)
                    .tabItem { Label("USB", systemImage: "cable.connector") }
                    .tag("usb")

                Form { SharingSection(sharing: $config.sharing, password: $vncPassword) }
                    .settingsPage()
                    .disabled(running)
                    .tabItem { Label("Sharing", systemImage: "rectangle.on.rectangle") }
                    .tag("sharing")

                Form { InputSection(config: $config) }
                    .settingsPage()
                    .disabled(running)
                    .tabItem { Label("Input", systemImage: "keyboard") }
                    .tag("input")
            }
            .padding([.top, .horizontal])

            Divider().padding(.top, 8)
            HStack {
                if running {
                    Label("Shut down to change hardware",
                          systemImage: "lock.fill")
                        .foregroundStyle(.secondary)
                } else if let error {
                    Label(error, systemImage: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange).lineLimit(2)
                }
                Spacer()
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button {
                    Task { await save() }
                } label: {
                    if saving { ProgressView().controlSize(.small) } else { Text("Save") }
                }
                .keyboardShortcut(.defaultAction)
                .disabled(saving || config.name.trimmingCharacters(in: .whitespaces).isEmpty)
            }
            .padding(16)
        }
        .frame(width: 720, height: 560)
    }

    private func save() async {
        saving = true
        defer { saving = false }
        error = nil
        do {
            for (id, gb) in newDisks {
                guard let i = config.drives.firstIndex(where: { $0.id == id }) else { continue }
                let url = vm.folder.appendingPathComponent(config.drives[i].file)
                try await Toolkit.createDisk(at: url, gigabytes: gb)
            }
            try vm.update(config)
            Secrets.setVNCPassword(vncPassword, for: config.uuid)
            dismiss()
        } catch {
            self.error = error.localizedDescription
        }
    }
}

/// Hard disks, CD/DVD drives and USB drive images.
struct StorageEditor: View {
    @Binding var config: VMConfig
    @Binding var newDisks: [String: Int]
    let folder: URL
    /// the drive the open panel chooses an image for (id, or "" for a new
    /// one); kept apart from whether it is open, which SwiftUI resets first
    @State private var choosing: (String, Drive.Kind) = ("", .disk)
    @State private var importing = false
    @State private var newDiskGB = 20
    @State private var remote: RemoteImage?

    struct RemoteImage: Identifiable {
        let id = UUID()
        var type: Drive.Kind
        var url: String

        var isValid: Bool {
            guard let u = URL(string: url), let host = u.host, !host.isEmpty,
                  u.path.count > 1 else { return false }
            return type == .cdrom ? ["https", "http"].contains(u.scheme ?? "")
                                  : u.scheme == "ssh"
        }
    }
    @State private var askingSize = false

    var body: some View {
        Form {
            Section {
                ForEach($config.drives) { $drive in
                    row(for: $drive)
                }
                .onMove { config.drives.move(fromOffsets: $0, toOffset: $1) }
                HStack {
                    Menu("Add Drive") {
                        Button("New Hard Disk\u{2026}") { askingSize = true }
                        Button("Existing Hard Disk Image\u{2026}") { choose("", .disk) }
                        Button("CD/DVD Drive") {
                            config.drives.append(Drive(id: nextID("cdrom"), type: .cdrom, file: ""))
                        }
                        Button("USB Drive Image\u{2026}") { choose("", .usb) }
                        Divider()
                        Button("Remote Disk Image (SSH)\u{2026}") {
                            remote = RemoteImage(type: .disk, url: "ssh://")
                        }
                        Button("Remote Disc Image (HTTPS)\u{2026}") {
                            remote = RemoteImage(type: .cdrom, url: "https://")
                        }
                    }
                    .fixedSize()
                    Spacer()
                }
            } header: {
                Text("Drives")
            }
            Section {
                Picker("Start up from", selection: $config.boot) {
                    ForEach(BootDevice.allCases) { Text($0.title).tag($0) }
                }
            }
        }
        .formStyle(.grouped)
        .sheet(isPresented: $askingSize) {
            VStack(alignment: .leading, spacing: 16) {
                Text("New Hard Disk").font(.headline)
                NumberField(title: "Size", value: $newDiskGB,
                            range: 1...Toolkit.freeDiskGB, unit: "GB")
                HStack {
                    Spacer()
                    Button("Cancel") { askingSize = false }.keyboardShortcut(.cancelAction)
                    Button("Add") {
                        let id = nextID("disk")
                        let file = uniqueFile("Disk \(config.drives.filter { $0.type == .disk }.count + 1).qcow2")
                        config.drives.append(Drive(id: id, type: .disk, file: file))
                        newDisks[id] = newDiskGB
                        askingSize = false
                    }
                    .keyboardShortcut(.defaultAction)
                }
            }
            .padding(20)
            .frame(width: 460)
        }
        .sheet(item: $remote) { r in
            RemoteImageSheet(image: r) { added in
                config.drives.append(Drive(id: nextID(added.type == .cdrom ? "cdrom" : "disk"),
                                           type: added.type, file: added.url,
                                           readOnly: added.type == .cdrom))
                remote = nil
            } cancel: {
                remote = nil
            }
        }
        .fileImporter(isPresented: $importing, allowedContentTypes: imageTypes) { result in
            guard case .success(let url) = result else { return }
            let (id, kind) = choosing
            if let i = config.drives.firstIndex(where: { $0.id == id }) {
                config.drives[i].file = url.path
            } else {
                config.drives.append(Drive(id: nextID(kind == .usb ? "usb" : kind.rawValue),
                                           type: kind, file: url.path))
            }
        }
    }

    @ViewBuilder
    private func row(for drive: Binding<Drive>) -> some View {
        let d = drive.wrappedValue
        HStack {
            Image(systemName: DriveRow.symbol(d.type))
                .frame(width: 22)
                .foregroundStyle(.secondary)
            VStack(alignment: .leading) {
                Text(DriveRow.title(d.type))
                Text(newDisks[d.id].map { "\(d.file) (new, \($0) GB)" }
                     ?? d.displayName(in: folder))
                    .font(.caption).foregroundStyle(.secondary)
                    .lineLimit(1).truncationMode(.middle)
            }
            Spacer()
            if d.type == .cdrom {
                Button("Choose\u{2026}") { choose(d.id, .cdrom) }
                if !d.file.isEmpty {
                    Button("Eject") { drive.wrappedValue.file = "" }
                }
            }
            if d.type != .cdrom {
                Toggle("Read-only", isOn: drive.readOnly)
                    .toggleStyle(.checkbox)
            }
            Button(role: .destructive) {
                newDisks[d.id] = nil
                config.drives.removeAll { $0.id == d.id }
            } label: {
                Image(systemName: "minus.circle")
            }
            .buttonStyle(.borderless)
            .help("Remove this drive (the image file stays in the folder)")
        }
    }

    private func choose(_ id: String, _ kind: Drive.Kind) {
        choosing = (id, kind)
        importing = true
    }

    private func nextID(_ prefix: String) -> String {
        var n = 0
        while config.drives.contains(where: { $0.id == "\(prefix)\(n)" }) { n += 1 }
        return "\(prefix)\(n)"
    }

    private func uniqueFile(_ name: String) -> String {
        var candidate = name
        var n = 2
        let base = (name as NSString).deletingPathExtension
        while FileManager.default.fileExists(atPath: folder.appendingPathComponent(candidate).path)
              || config.drives.contains(where: { $0.file == candidate }) {
            candidate = "\(base) \(n).qcow2"
            n += 1
        }
        return candidate
    }
}

extension View {
    /// A settings tab's form: grouped, over the tab view's own background
    /// rather than a darker one of its own inside it.
    func settingsPage() -> some View {
        formStyle(.grouped)
            .scrollContentBackground(.hidden)
    }
}

/// A disk image over SSH or a disc image over HTTPS, which QEMU opens
/// itself: SSH uses this Mac's ssh-agent keys and known_hosts.
struct RemoteImageSheet: View {
    @State var image: StorageEditor.RemoteImage
    var add: (StorageEditor.RemoteImage) -> Void
    var cancel: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text(image.type == .cdrom ? "Remote Disc Image" : "Remote Disk Image")
                .font(.headline)
            TextField(image.type == .cdrom ? "https://server/path/disc.iso"
                                           : "ssh://user@server/path/disk.qcow2",
                      text: $image.url)
                .textFieldStyle(.roundedBorder)
                .frame(minWidth: 380)
            HStack {
                Spacer()
                Button("Cancel", action: cancel).keyboardShortcut(.cancelAction)
                Button("Add") { add(image) }
                    .keyboardShortcut(.defaultAction)
                    .disabled(!image.isValid)
            }
        }
        .padding(20)
    }
}
