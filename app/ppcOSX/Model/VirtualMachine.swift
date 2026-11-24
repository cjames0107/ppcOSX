import AppKit
import Foundation

enum VMState: Equatable {
    case stopped, starting, running, paused, stopping

    var title: String {
        switch self {
        case .stopped: return "Stopped"
        case .starting: return "Starting"
        case .running: return "Running"
        case .paused: return "Paused"
        case .stopping: return "Shutting down"
        }
    }

    var isActive: Bool { self != .stopped }
}

/// One VM: a folder in the library holding config.json, its disk images
/// and qemu.log. While it runs, QEMU (started through ppcosx) is a child
/// process, controlled over a QMP socket.
@MainActor
final class VirtualMachine: ObservableObject, Identifiable {
    @Published private(set) var folder: URL
    @Published private(set) var config: VMConfig
    @Published private(set) var state: VMState = .stopped
    @Published var lastError: String?
    /// the drive being compacted, if any
    @Published private(set) var compacting: String?
    private var passwordSent = false

    /// the screen while running (QEMU's ppcosx display), and its window
    private(set) var display: DisplayClient?
    private var windowController: VMWindowController?
    /// discs, connected USB devices and the network cable, as last asked
    private(set) var devices = LiveDevices()

    private var process: Process?
    /// QEMU's pid: our child's, or one found running (pid file)
    private var pid: pid_t?
    private var exitSource: DispatchSourceProcess?
    private var pollTimer: Timer?
    private var startedAt = Date()
    /// short, as Unix socket paths are limited to ~100 bytes
    let socketPath: String
    let displayPath: String

    nonisolated var id: UUID { _id }
    private let _id: UUID

    init(folder: URL, config: VMConfig) {
        self.folder = folder
        self.config = config
        self._id = config.uuid
        socketPath = (NSTemporaryDirectory() as NSString)
            .appendingPathComponent("ppcosx-\(config.uuid.uuidString.prefix(8)).sock")
        displayPath = (NSTemporaryDirectory() as NSString)
            .appendingPathComponent("ppcosx-\(config.uuid.uuidString.prefix(8)).display")
        attachIfRunning()
    }

    /// A VM still running from an earlier session of the app: its QEMU
    /// wrote qemu.pid in the folder (and removes it on exit).
    private func attachIfRunning() {
        guard let text = try? String(contentsOf: folder.appendingPathComponent("qemu.pid"), encoding: .utf8),
              let found = pid_t(text.trimmingCharacters(in: .whitespacesAndNewlines)),
              kill(found, 0) == 0 else { return }
        pid = found
        state = .running
        watchExit(of: found)
        startPolling()
        connectDisplay()
    }

    private func connectDisplay() {
        display?.stop()
        let client = DisplayClient(path: displayPath)
        client.start()
        display = client
        devices = LiveDevices()
    }

    private func watchExit(of pid: pid_t) {
        let source = DispatchSource.makeProcessSource(identifier: pid, eventMask: .exit,
                                                      queue: .main)
        source.setEventHandler { [weak self] in
            MainActor.assumeIsolated { self?.didExit(status: 0) }
        }
        source.resume()
        exitSource = source
    }

    private func startPolling() {
        pollTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            Task { @MainActor in await self?.poll() }
        }
    }

    static func load(from folder: URL) throws -> VirtualMachine {
        let data = try Data(contentsOf: folder.appendingPathComponent("config.json"))
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        return VirtualMachine(folder: folder,
                              config: try decoder.decode(VMConfig.self, from: data))
    }

    static func write(_ config: VMConfig, to folder: URL) throws {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        try encoder.encode(config).write(to: folder.appendingPathComponent("config.json"),
                                         options: .atomic)
    }

    var logURL: URL { folder.appendingPathComponent("qemu.log") }

    /// Total size of the VM's own disk images, in bytes.
    var diskUsage: Int64 {
        config.drives.compactMap { $0.url(in: folder) }
            .filter { $0.path.hasPrefix(folder.path) }
            .compactMap { try? $0.resourceValues(forKeys: [.totalFileAllocatedSizeKey]) }
            .reduce(0) { $0 + Int64($1.totalFileAllocatedSize ?? 0) }
    }

    // MARK: settings

    /// Save new settings; a new name renames the folder too (when stopped).
    func update(_ new: VMConfig) throws {
        var newFolder = folder
        if new.name != config.name && !state.isActive {
            let candidate = folder.deletingLastPathComponent()
                .appendingPathComponent(VMLibrary.folderName(for: new.name))
            if !FileManager.default.fileExists(atPath: candidate.path) {
                try FileManager.default.moveItem(at: folder, to: candidate)
                newFolder = candidate
            }
        }
        try VirtualMachine.write(new, to: newFolder)
        folder = newFolder
        config = new
    }

    func setNotes(_ notes: String) {
        var c = config
        c.notes = notes
        try? update(c)
    }

    // MARK: running

    func start() {
        guard !state.isActive else { return }
        lastError = nil
        let command: Launcher.Command
        do {
            try? FileManager.default.removeItem(atPath: socketPath)
            if config.sharing.enabled && config.sharing.tls {
                try TLSCertificates.ensure(in: folder.appendingPathComponent("tls"))
            }
            passwordSent = false
            command = try Launcher.command(folder: folder, config: config, socket: socketPath,
                                           display: displayPath)
        } catch {
            lastError = error.localizedDescription
            return
        }
        FileManager.default.createFile(atPath: logURL.path, contents: nil)
        let log = try? FileHandle(forWritingTo: logURL)
        if let log {
            let line = ([command.executable.path] + command.arguments)
                .map { $0.contains(" ") ? "'\($0)'" : $0 }.joined(separator: " ") + "\n"
            log.write(Data(line.utf8))
        }
        let p = Process()
        p.executableURL = command.executable
        p.arguments = command.arguments
        p.currentDirectoryURL = folder
        p.standardOutput = log
        p.standardError = log
        p.terminationHandler = { [weak self] proc in
            let status = proc.terminationStatus
            Task { @MainActor in self?.didExit(status: status) }
        }
        do {
            try p.run()
        } catch {
            lastError = error.localizedDescription
            return
        }
        process = p
        pid = p.processIdentifier
        startedAt = Date()
        state = .starting
        startPolling()
        connectDisplay()
        showWindow()
    }

    private func didExit(status: Int32) {
        guard state != .stopped else { return }
        pollTimer?.invalidate()
        pollTimer = nil
        exitSource?.cancel()
        exitSource = nil
        process = nil
        pid = nil
        display?.stop()
        display = nil
        // QEMU exits at once with an error (a bad option...), and
        // with 0 when the guest powers off
        if status != 0 && state != .stopping {
            let tail = (try? String(contentsOf: logURL, encoding: .utf8))?
                .split(separator: "\n").suffix(4).joined(separator: "\n") ?? ""
            lastError = "The virtual machine stopped unexpectedly (status \(status)).\n\(tail)"
        }
        state = .stopped
        try? FileManager.default.removeItem(atPath: socketPath)
    }

    private func poll() async {
        guard pid != nil, state != .stopping else { return }
        guard let r = try? await QMP.execute(socket: socketPath, "query-status")
                as? [String: Any] else { return }
        let running = r["running"] as? Bool ?? false
        if state != .stopping {
            state = running ? .running : .paused
        }
        // VNC was started with password=on: give it the password
        if config.sharing.enabled && !passwordSent {
            passwordSent = true
            if let pw = Secrets.vncPassword(for: config.uuid) {
                _ = try? await QMP.execute(socket: socketPath, "change-vnc-password",
                                           arguments: ["password": pw])
            }
        }
    }

    // MARK: screenshots and disks

    /// A PNG of the screen on the Desktop, named like macOS's screenshots.
    func screenshot() {
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd 'at' HH.mm.ss"
        let desktop = FileManager.default.urls(for: .desktopDirectory, in: .userDomainMask)[0]
        let file = desktop.appendingPathComponent("\(config.name) \(f.string(from: Date())).png")
        let path = socketPath
        Task {
            do {
                try await QMP.execute(socket: path, "screendump",
                                      arguments: ["filename": file.path, "format": "png"])
                NSWorkspace.shared.activateFileViewerSelecting([file])
            } catch {
                lastError = error.localizedDescription
            }
        }
    }

    /// Local qcow2 disks can be compacted while the VM is stopped.
    func canCompact(_ drive: Drive) -> Bool {
        guard !state.isActive, compacting == nil, drive.type != .cdrom, !drive.isRemote,
              let url = drive.url(in: folder) else { return false }
        return url.pathExtension == "qcow2" && FileManager.default.fileExists(atPath: url.path)
    }

    /// Rewrite a disk image with its clusters compressed (zstd): it takes
    /// less space until the guest rewrites them.
    func compact(_ drive: Drive) {
        guard canCompact(drive), let url = drive.url(in: folder),
              let qemuImg = Toolkit.qemuImg else { return }
        compacting = drive.id
        lastError = nil
        let tmp = url.deletingLastPathComponent()
            .appendingPathComponent(".\(url.lastPathComponent).compacting")
        Task {
            defer { compacting = nil }
            do {
                try? FileManager.default.removeItem(at: tmp)
                try await Toolkit.run(qemuImg, ["convert", "-q", "-c", "-O", "qcow2",
                                                "-o", "compression_type=zstd",
                                                url.path, tmp.path])
                _ = try FileManager.default.replaceItemAt(url, withItemAt: tmp)
                objectWillChange.send()
            } catch {
                try? FileManager.default.removeItem(at: tmp)
                lastError = "Compacting \(url.lastPathComponent) failed: \(error.localizedDescription)"
            }
        }
    }

    func pause() { command("stop") { $0.state = .paused } }
    func resume() { command("cont") { $0.state = .running } }

    /// the guest's power key: Mac OS X asks to shut down, or shuts down
    func shutDown() { command("system_powerdown") { _ in } }

    func restart() { command("system_reset") { _ in } }

    func forceStop() {
        state = .stopping
        command("quit") { _ in }
        // if QMP does not answer, stop the process
        let target = pid
        DispatchQueue.main.asyncAfter(deadline: .now() + 5) {
            if let target, kill(target, 0) == 0 { kill(target, SIGTERM) }
        }
    }

    /// Open the VM's window, or bring it to the front.
    func showWindow() {
        guard let display, state.isActive else { return }
        if windowController == nil {
            windowController = VMWindowController(vm: self, client: display)
        }
        windowController?.showWindow(nil)
        windowController?.window?.makeKeyAndOrderFront(nil)
        NSApp.activate()
    }

    var hasWindow: Bool { windowController?.window?.isVisible == true }

    func windowClosed() {
        windowController = nil
        if VMFocus.shared.machine === self { VMFocus.shared.machine = nil }
    }

    func windowBecameKey() {
        VMFocus.shared.machine = self
    }

    func toggleCapture() {
        windowController?.toggleCapture(nil)
    }

    // MARK: devices

    struct LiveDevices {
        /// CD/DVD drive id -> the disc image in it (nil: empty)
        var media: [String: String?] = [:]
        /// qdev ids of connected host USB devices
        var usb: Set<String> = []
        var cableConnected = true
    }

    /// Ask QEMU what is in the drives and which USB devices are connected
    /// (briefly, on the main thread: for menus as they open).
    func refreshDevices() {
        guard state.isActive else { return }
        if let blocks = try? QMP.executeSync(socket: socketPath, "query-block", arguments: nil,
                                             timeout: 1) as? [[String: Any]] {
            for b in blocks {
                guard let dev = b["device"] as? String, b["removable"] as? Bool == true else { continue }
                let inserted = b["inserted"] as? [String: Any]
                devices.media[dev] = inserted?["file"] as? String
            }
        }
        if let list = try? QMP.executeSync(socket: socketPath, "qom-list",
                                           arguments: ["path": "/machine/peripheral"],
                                           timeout: 1) as? [[String: Any]] {
            devices.usb = Set(list.compactMap { $0["name"] as? String }
                                  .filter { $0.hasPrefix("usb-") })
        }
    }

    func chooseDisc(_ drive: String) {
        let panel = NSOpenPanel()
        panel.title = "Choose a Disc Image"
        panel.allowedContentTypes = [.diskImage, .data]
        panel.allowsOtherFileTypes = true
        guard panel.runModal() == .OK, let url = panel.url else { return }
        command("blockdev-change-medium", ["device": drive, "filename": url.path,
                                           "format": "raw"])
    }

    func eject(_ drive: String) {
        command("eject", ["device": drive, "force": true])
    }

    func setUSB(_ d: USBPassthrough, attached: Bool) {
        if attached {
            command("device_add", ["driver": "usb-host", "id": d.qdevID,
                                   "vendorid": d.vendor, "productid": d.product])
        } else {
            command("device_del", ["id": d.qdevID])
        }
    }

    func setCable(connected: Bool) {
        devices.cableConnected = connected
        command("set_link", ["name": "net0", "up": connected])
    }

    private func command(_ name: String, _ args: [String: Any]) {
        let path = socketPath
        Task {
            do {
                try await QMP.execute(socket: path, name, arguments: args)
            } catch {
                lastError = error.localizedDescription
                NSSound.beep()
            }
        }
    }

    private func command(_ name: String, then: @escaping (VirtualMachine) -> Void) {
        let path = socketPath
        Task {
            do {
                try await QMP.execute(socket: path, name)
                then(self)
            } catch {
                if name != "quit" { lastError = error.localizedDescription }
            }
        }
    }
}

/// What the menu bar's Virtual Machine menu acts on: the VM whose window
/// is in front, else the one selected in the library.
@MainActor
final class VMFocus: ObservableObject {
    static let shared = VMFocus()
    /// the VM whose window is key (nil when another window is)
    @Published var machine: VirtualMachine?
    /// the library's selection
    @Published var selected: VirtualMachine?

    var target: VirtualMachine? { machine ?? selected }

    private init() {
        NotificationCenter.default.addObserver(forName: NSWindow.didBecomeKeyNotification,
                                               object: nil, queue: .main) { note in
            let vm = ((note.object as? NSWindow)?.windowController as? VMWindowController)?.vm
            MainActor.assumeIsolated { VMFocus.shared.machine = vm }
        }
    }
}
