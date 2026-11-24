import SwiftUI
import UniformTypeIdentifiers

/// Install media and disk image types: ISO/CDR images and raw disk images.
let imageTypes: [UTType] = ["iso", "cdr", "img", "toast", "dmg", "qcow2", "raw"]
    .compactMap { UTType(filenameExtension: $0) } + [.diskImage, .data]

struct NewVMWizard: View {
    enum Step: Int, CaseIterable {
        case system, media, hardware, storage, network, finish

        var title: String {
            switch self {
            case .system: return "Name and Operating System"
            case .media: return "Install Media"
            case .hardware: return "Hardware"
            case .storage: return "Storage"
            case .network: return "Network and Sound"
            case .finish: return "Summary"
            }
        }
    }

    @EnvironmentObject var library: VMLibrary
    @Environment(\.dismiss) private var dismiss
    var created: (VirtualMachine) -> Void
    /// close the wizard and add a VM folder made elsewhere instead
    var addExisting: () -> Void = {}

    @State private var step: Step = .system
    @State private var config = VMConfig(name: "")
    @State private var installMedia: URL?
    @State private var diskGB = 40
    @State private var useExistingDisk = false
    @State private var existingDisk: URL?
    /// what the open panel is choosing for; kept apart from whether it is
    /// open, as SwiftUI closes it (and resets that) before the result comes
    @State private var choosing: Chooser = .media
    @State private var importing = false
    @State private var creating = false
    @State private var error: String?
    /// the name follows the system until the user types one
    @State private var nameEdited = false

    enum Chooser: Identifiable {
        case media, disk
        var id: Int { hashValue }
    }

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            Group {
                switch step {
                case .system: systemStep
                case .media: mediaStep
                case .hardware: hardwareStep
                case .storage: storageStep
                case .network: networkStep
                case .finish: finishStep
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            Divider()
            footer
        }
        .frame(width: 640, height: 560)
        .onAppear { applyDefaults(for: config.guest) }
        .fileImporter(isPresented: $importing, allowedContentTypes: imageTypes) { result in
            if case .success(let url) = result {
                switch choosing {
                case .disk: existingDisk = url
                case .media: installMedia = url
                }
            }
        }
    }

    private var header: some View {
        HStack(spacing: 14) {
            GuestIcon(guest: config.guest, size: 40)
            VStack(alignment: .leading, spacing: 2) {
                Text("New Virtual Machine").font(.headline)
                Text("Step \(step.rawValue + 1) of \(Step.allCases.count): \(step.title)")
                    .font(.subheadline).foregroundStyle(.secondary)
            }
            Spacer()
        }
        .padding(16)
    }

    private var footer: some View {
        HStack {
            if let error {
                Label(error, systemImage: "exclamationmark.triangle.fill")
                    .foregroundStyle(.orange)
                    .lineLimit(2)
            }
            Spacer()
            Button("Cancel") { dismiss() }
                .keyboardShortcut(.cancelAction)
            if step != .system {
                Button("Back") { step = Step(rawValue: step.rawValue - 1)! }
            }
            if step == .finish {
                Button {
                    Task { await create() }
                } label: {
                    if creating { ProgressView().controlSize(.small) } else { Text("Create") }
                }
                .keyboardShortcut(.defaultAction)
                .disabled(creating || config.name.trimmingCharacters(in: .whitespaces).isEmpty)
            } else {
                Button("Continue") { step = Step(rawValue: step.rawValue + 1)! }
                    .keyboardShortcut(.defaultAction)
                    .disabled((step == .storage && useExistingDisk && existingDisk == nil) ||
                              (step == .system && config.name.trimmingCharacters(in: .whitespaces).isEmpty))
            }
        }
        .padding(16)
    }

    // MARK: steps

    private var systemStep: some View {
        Form {
            Section {
                TextField("Name", text: Binding(get: { config.name },
                                                 set: { config.name = $0; nameEdited = true }))
                Picker("Operating system", selection: $config.guest) {
                    ForEach(GuestOS.allCases) { g in
                        Label(g.title, systemImage: g.symbol).tag(g)
                    }
                }
                .pickerStyle(.menu)
                .onChange(of: config.guest) { _, g in applyDefaults(for: g) }
            } header: {
                Text("Name the virtual machine and choose the system you will install")
            }
            Section("Recommended hardware") {
                LabeledContent("Processor", value: config.cpu.title)
                LabeledContent("Memory", value: MemoryLimits.format(config.memory))
                LabeledContent("Graphics", value: config.graphics.title)
                LabeledContent("Acceleration", value: config.graphics.features(for: config.guest))
                LabeledContent("Hard disk", value: "\(diskGB) GB")
            }
            Section {
                LabeledContent("Already have one?") {
                    Button("Add Existing Virtual Machine\u{2026}") {
                        dismiss()
                        addExisting()
                    }
                }
            }
        }
        .formStyle(.grouped)
    }

    private var mediaStep: some View {
        Form {
            Section {
                LabeledContent("Install disc") {
                    HStack {
                        Text(installMedia?.lastPathComponent ?? "None")
                            .foregroundStyle(installMedia == nil ? .secondary : .primary)
                            .lineLimit(1).truncationMode(.middle)
                        Button("Choose\u{2026}") { choosing = .media; importing = true }
                        if installMedia != nil {
                            Button("Clear") { installMedia = nil }
                        }
                    }
                }
            } header: {
                Text("Install media")
            }
        }
        .formStyle(.grouped)
    }

    private var hardwareStep: some View {
        Form {
            SystemSection(config: $config)
            DisplaySection(config: $config)
        }
        .formStyle(.grouped)
    }

    private var storageStep: some View {
        Form {
            Section {
                Picker("Hard disk", selection: $useExistingDisk) {
                    Text("Create a new disk").tag(false)
                    Text("Use an existing disk image").tag(true)
                }
                .pickerStyle(.radioGroup)
                if useExistingDisk {
                    LabeledContent("Disk image") {
                        HStack {
                            Text(existingDisk?.lastPathComponent ?? "None")
                                .foregroundStyle(existingDisk == nil ? .secondary : .primary)
                                .lineLimit(1).truncationMode(.middle)
                            Button("Choose\u{2026}") { choosing = .disk; importing = true }
                        }
                    }
                } else {
                    NumberField(title: "Size", value: $diskGB,
                                range: 1...Toolkit.freeDiskGB, unit: "GB")
                }
            } header: {
                Text("Storage")
            }
        }
        .formStyle(.grouped)
    }

    private var networkStep: some View {
        Form {
            NetworkSection(network: $config.network)
            SoundSection(audio: $config.audio)
            InputSection(config: $config)
        }
        .formStyle(.grouped)
    }

    private var finishStep: some View {
        Form {
            Section {
                LabeledContent("Name", value: config.name)
                LabeledContent("System", value: config.guest.title)
                LabeledContent("Processor",
                               value: "\(config.cpu.title), \(config.cores) core\(config.cores == 1 ? "" : "s")")
                LabeledContent("Memory", value: MemoryLimits.format(config.memory))
                LabeledContent("Graphics", value: config.graphics.title)
                LabeledContent("Hard disk", value: useExistingDisk
                               ? (existingDisk?.lastPathComponent ?? "None") : "New, \(diskGB) GB")
                LabeledContent("Install disc", value: installMedia?.lastPathComponent ?? "None")
                LabeledContent("Network", value: config.network.mode.title)
            } header: {
                Text("Ready to create")
            }
        }
        .formStyle(.grouped)
    }

    // MARK: actions

    private func applyDefaults(for guest: GuestOS) {
        if !guest.cpus.contains(config.cpu) { config.cpu = guest.cpus.contains(.g4) ? .g4 : guest.cpus[0] }
        config.cores = min(config.cores, config.cpu.maxCores)
        config.memory = guest.recommendedMemory
        config.graphics = guest.recommendedGraphics
        config.input = .auto
        diskGB = min(guest.recommendedDisk, Toolkit.freeDiskGB)
        if !nameEdited || config.name.trimmingCharacters(in: .whitespaces).isEmpty {
            config.name = library.uniqueName(guest == .other ? "Virtual Machine" : guest.shortTitle)
            nameEdited = false
        }
    }

    private func create() async {
        error = nil
        creating = true
        defer { creating = false }
        do {
            let vm = try await library.create(config, diskGB: useExistingDisk ? 0 : diskGB,
                                              existingDisk: useExistingDisk ? existingDisk : nil,
                                              installMedia: installMedia)
            created(vm)
            dismiss()
        } catch {
            self.error = error.localizedDescription
        }
    }
}
