import Foundation

/// The emulator inside the app (tools/bundle-emulator.sh puts it there at
/// build time): QEMU and qemu-img in Resources/qemu/bin, their firmware in
/// Resources/qemu/share/qemu, the Open Firmware Forth generators and machine
/// tables in Resources/scripts, their libraries in Frameworks.
enum Toolkit {
    static let libraryKey = "libraryPath"

    static var emulatorBin: URL? {
        existing(Bundle.main.resourceURL?.appendingPathComponent("qemu/bin"))
    }

    static var scripts: URL? {
        existing(Bundle.main.resourceURL?.appendingPathComponent("scripts"))
    }

    static var qemuImg: URL? {
        existing(emulatorBin?.appendingPathComponent("qemu-img"))
    }

    static var isComplete: Bool {
        emulatorBin != nil && scripts != nil && qemuImg != nil && Launcher.machineData != nil
    }

    private static func existing(_ url: URL?) -> URL? {
        guard let url, FileManager.default.fileExists(atPath: url.path) else { return nil }
        return url
    }

    /// ~/Documents/PPC VMs, unless the user picked another folder
    static var libraryFolder: URL {
        if let path = UserDefaults.standard.string(forKey: libraryKey),
           !path.isEmpty {
            return URL(fileURLWithPath: path)
        }
        return FileManager.default.urls(for: .documentDirectory,
                                        in: .userDomainMask)[0]
            .appendingPathComponent("PPC VMs")
    }

    /// Run a tool to completion off the main thread; throws with its output
    /// if it fails.
    static func run(_ tool: URL, _ args: [String]) async throws {
        try await Task.detached {
            let p = Process()
            let pipe = Pipe()
            p.executableURL = tool
            p.arguments = args
            p.standardOutput = pipe
            p.standardError = pipe
            try p.run()
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            p.waitUntilExit()
            if p.terminationStatus != 0 {
                let out = String(decoding: data, as: UTF8.self)
                throw ToolError(message: "\(tool.lastPathComponent) failed: \(out)")
            }
        }.value
    }

    static func createDisk(at url: URL, gigabytes: Int) async throws {
        guard let qemuImg else {
            throw ToolError(message: "qemu-img is missing from the application.")
        }
        // zstd for clusters written compressed (Compact Disk)
        try await run(qemuImg, ["create", "-q", "-f", "qcow2", "-o", "compression_type=zstd",
                                url.path, "\(gigabytes)G"])
    }

    /// The host's memory, in MiB.
    static var hostMemoryMB: Int {
        Int(ProcessInfo.processInfo.physicalMemory / (1024 * 1024))
    }

    /// Free space where the VMs live, in GB (what macOS can make available).
    static var freeDiskGB: Int {
        var dir = libraryFolder
        while !FileManager.default.fileExists(atPath: dir.path) && dir.path != "/" {
            dir = dir.deletingLastPathComponent()
        }
        let values = try? dir.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey,
                                                       .volumeAvailableCapacityKey])
        let bytes = values?.volumeAvailableCapacityForImportantUsage
            ?? Int64(values?.volumeAvailableCapacity ?? 0)
        return max(Int(bytes / 1_000_000_000), 1)
    }
}

struct ToolError: LocalizedError {
    var message: String
    var errorDescription: String? { message }
}
