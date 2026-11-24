import Foundation

/// The VM library: one folder per VM under ~/Documents/PPC VMs.
@MainActor
final class VMLibrary: ObservableObject {
    @Published private(set) var machines: [VirtualMachine] = []
    @Published var error: String?

    var root: URL { Toolkit.libraryFolder }

    init() {
        reload()
    }

    func reload() {
        let fm = FileManager.default
        try? fm.createDirectory(at: root, withIntermediateDirectories: true)
        let folders = (try? fm.contentsOfDirectory(at: root, includingPropertiesForKeys: nil,
                                                   options: [.skipsHiddenFiles])) ?? []
        var found: [VirtualMachine] = []
        for folder in folders where fm.fileExists(atPath: folder.appendingPathComponent("config.json").path) {
            // keep the objects of VMs already known (they may be running)
            if let known = machines.first(where: { $0.folder.standardizedFileURL == folder.standardizedFileURL }) {
                found.append(known)
            } else if let vm = try? VirtualMachine.load(from: folder) {
                found.append(vm)
            }
        }
        machines = sorted(found)
    }

    // MARK: order

    private static let orderKey = "libraryOrder"

    /// The sidebar's order, as the user arranged it: VM ids, saved in the
    /// app's preferences. VMs not in it (new or added) follow, by name.
    private var savedOrder: [UUID] {
        get {
            (UserDefaults.standard.stringArray(forKey: VMLibrary.orderKey) ?? [])
                .compactMap(UUID.init(uuidString:))
        }
        set {
            UserDefaults.standard.set(newValue.map(\.uuidString), forKey: VMLibrary.orderKey)
        }
    }

    private func sorted(_ vms: [VirtualMachine]) -> [VirtualMachine] {
        let order = savedOrder
        return vms.sorted { a, b in
            switch (order.firstIndex(of: a.id), order.firstIndex(of: b.id)) {
            case let (i?, j?): return i < j
            case (_?, nil): return true
            case (nil, _?): return false
            default:
                return a.config.name.localizedStandardCompare(b.config.name) == .orderedAscending
            }
        }
    }

    func move(from offsets: IndexSet, to destination: Int) {
        machines.move(fromOffsets: offsets, toOffset: destination)
        savedOrder = machines.map(\.id)
    }

    /// A folder name from a VM name: no slashes or colons.
    static func folderName(for name: String) -> String {
        let cleaned = name.replacingOccurrences(of: "/", with: "-")
            .replacingOccurrences(of: ":", with: "-")
            .trimmingCharacters(in: .whitespacesAndNewlines)
        return cleaned.isEmpty ? "Untitled" : cleaned
    }

    func uniqueName(_ base: String) -> String {
        var name = base
        var n = 2
        while machines.contains(where: { $0.config.name == name }) ||
              FileManager.default.fileExists(atPath: root.appendingPathComponent(
                VMLibrary.folderName(for: name)).path) {
            name = "\(base) \(n)"
            n += 1
        }
        return name
    }

    /// Make a VM's folder, its new hard disk (if `diskGB` > 0) and config.
    func create(_ draft: VMConfig, diskGB: Int, existingDisk: URL?,
                installMedia: URL?) async throws -> VirtualMachine {
        var config = draft
        config.uuid = UUID()
        config.created = Date()
        let folder = root.appendingPathComponent(VMLibrary.folderName(for: config.name))
        let fm = FileManager.default
        guard !fm.fileExists(atPath: folder.path) else {
            throw ToolError(message: "A folder named \u{201c}\(folder.lastPathComponent)\u{201d} already exists in \(root.lastPathComponent).")
        }
        try fm.createDirectory(at: folder, withIntermediateDirectories: true)
        do {
            var drives: [Drive] = []
            if let existingDisk {
                drives.append(Drive(id: "disk0", type: .disk, file: existingDisk.path))
            } else if diskGB > 0 {
                let disk = folder.appendingPathComponent("Macintosh HD.qcow2")
                try await Toolkit.createDisk(at: disk, gigabytes: diskGB)
                drives.append(Drive(id: "disk0", type: .disk, file: disk.lastPathComponent))
            }
            drives.append(Drive(id: "cdrom0", type: .cdrom, file: installMedia?.path ?? ""))
            config.drives = drives
            config.boot = installMedia != nil && existingDisk == nil ? .cdrom : .disk
            try VirtualMachine.write(config, to: folder)
        } catch {
            try? fm.removeItem(at: folder)
            throw error
        }
        let vm = VirtualMachine(folder: folder, config: config)
        machines.append(vm)
        savedOrder = machines.map(\.id)       // new VMs go at the end
        return vm
    }

    /// Move a stopped VM's folder to the Trash.
    func delete(_ vm: VirtualMachine) throws {
        guard !vm.state.isActive else {
            throw ToolError(message: "Stop \u{201c}\(vm.config.name)\u{201d} before deleting it.")
        }
        try FileManager.default.trashItem(at: vm.folder, resultingItemURL: nil)
        machines.removeAll { $0.id == vm.id }
        savedOrder = machines.map(\.id)
    }

    /// Copy another VM folder (from elsewhere) into the library.
    func importFolder(_ url: URL) throws -> VirtualMachine {
        let vm = try VirtualMachine.load(from: url)
        let dest = root.appendingPathComponent(url.lastPathComponent)
        if url.standardizedFileURL != dest.standardizedFileURL {
            try FileManager.default.copyItem(at: url, to: dest)
        }
        reload()
        return machines.first { $0.folder.standardizedFileURL == dest.standardizedFileURL } ?? vm
    }

    var anyRunning: Bool { machines.contains { $0.state.isActive } }
}
