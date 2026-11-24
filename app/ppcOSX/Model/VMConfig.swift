import Foundation

/// A virtual machine's settings: its folder's `config.json`, which
/// `ppcosx run --vm` reads to build the emulator's command line. Every key
/// is optional when reading, so files from older versions still load.
struct VMConfig: Codable, Equatable {
    /// 2: sound exists (version 1 files said off only as it was not emulated)
    var version = 2
    var name: String
    var uuid = UUID()
    var notes = ""
    var created = Date()
    var guest: GuestOS = .tiger
    var cpu: CPUType = .g4
    var cores = 1
    /// MiB
    var memory = 1024
    var graphics: GraphicsCard = .radeon9600
    /// the firmware's screen mode, as QEMU's -g takes it
    var resolution = "1024x768x32"
    var input: InputMode = .auto
    var boot: BootDevice = .disk
    var drives: [Drive] = []
    var network = Network()
    var audio = Audio()
    var sharing = Sharing()
    /// USB devices of this Mac handed to the guest when it starts
    var usb: [USBPassthrough] = []

    init(name: String) {
        self.name = name
    }

    enum CodingKeys: String, CodingKey {
        case version, name, uuid, notes, created, guest, cpu, cores, memory,
             graphics, resolution, input, boot, drives, network, audio, sharing, usb
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = VMConfig(name: "")
        version = try c.decodeIfPresent(Int.self, forKey: .version) ?? d.version
        name = try c.decodeIfPresent(String.self, forKey: .name) ?? "Untitled"
        uuid = try c.decodeIfPresent(UUID.self, forKey: .uuid) ?? UUID()
        notes = try c.decodeIfPresent(String.self, forKey: .notes) ?? ""
        created = try c.decodeIfPresent(Date.self, forKey: .created) ?? Date()
        guest = (try? c.decodeIfPresent(GuestOS.self, forKey: .guest)) ?? .other
        cpu = (try? c.decodeIfPresent(CPUType.self, forKey: .cpu)) ?? .g4
        cores = try c.decodeIfPresent(Int.self, forKey: .cores) ?? 1
        memory = try c.decodeIfPresent(Int.self, forKey: .memory) ?? 1024
        graphics = (try? c.decodeIfPresent(GraphicsCard.self, forKey: .graphics)) ?? .radeon9600
        resolution = try c.decodeIfPresent(String.self, forKey: .resolution) ?? d.resolution
        input = (try? c.decodeIfPresent(InputMode.self, forKey: .input)) ?? .auto
        boot = (try? c.decodeIfPresent(BootDevice.self, forKey: .boot)) ?? .disk
        drives = try c.decodeIfPresent([Drive].self, forKey: .drives) ?? []
        network = try c.decodeIfPresent(Network.self, forKey: .network) ?? Network()
        audio = try c.decodeIfPresent(Audio.self, forKey: .audio) ?? Audio()
        if version < 2 {
            audio.enabled = true
            version = 2
        }
        sharing = try c.decodeIfPresent(Sharing.self, forKey: .sharing) ?? Sharing()
        usb = try c.decodeIfPresent([USBPassthrough].self, forKey: .usb) ?? []
    }
}

struct Drive: Codable, Equatable, Identifiable {
    enum Kind: String, Codable, CaseIterable {
        case disk, cdrom, usb
    }

    /// the emulator's drive id ("disk0", "cdrom0"...), shown in its window
    var id: String
    var type: Kind
    /// relative to the VM folder, or absolute; empty for an empty CD drive
    var file: String
    var readOnly = false

    enum CodingKeys: String, CodingKey { case id, type, file, readOnly }

    init(id: String, type: Kind, file: String, readOnly: Bool = false) {
        self.id = id
        self.type = type
        self.file = file
        self.readOnly = readOnly
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        type = (try? c.decodeIfPresent(Kind.self, forKey: .type)) ?? .disk
        id = try c.decodeIfPresent(String.self, forKey: .id) ?? type.rawValue
        file = try c.decodeIfPresent(String.self, forKey: .file) ?? ""
        readOnly = try c.decodeIfPresent(Bool.self, forKey: .readOnly) ?? false
    }

    /// an image QEMU opens over the network (ssh://, https://)
    var isRemote: Bool { file.contains("://") }

    func url(in folder: URL) -> URL? {
        guard !file.isEmpty else { return nil }
        if isRemote { return URL(string: file) }
        return file.hasPrefix("/") ? URL(fileURLWithPath: file)
                                   : folder.appendingPathComponent(file)
    }

    /// what to show for it: a file name, or host and path for a remote one
    func displayName(in folder: URL) -> String {
        guard let url = url(in: folder) else { return "Empty" }
        if isRemote {
            return "\(url.host ?? "")\(url.path)"
        }
        return url.lastPathComponent
    }
}

struct Network: Codable, Equatable {
    enum Mode: String, Codable, CaseIterable, Identifiable {
        case nat, none
        var id: String { rawValue }
        var title: String {
            switch self {
            case .nat: return "Shared (NAT)"
            case .none: return "Not connected"
            }
        }
    }

    struct Forward: Codable, Equatable, Identifiable {
        var id = UUID()
        var proto = "tcp"
        var host: Int
        var guest: Int

        enum CodingKeys: String, CodingKey { case proto = "protocol", host, guest }

        init(proto: String = "tcp", host: Int, guest: Int) {
            self.proto = proto
            self.host = host
            self.guest = guest
        }

        init(from decoder: Decoder) throws {
            let c = try decoder.container(keyedBy: CodingKeys.self)
            proto = try c.decodeIfPresent(String.self, forKey: .proto) ?? "tcp"
            host = try c.decode(Int.self, forKey: .host)
            guest = try c.decode(Int.self, forKey: .guest)
        }
    }

    var mode: Mode = .nat
    /// the Ethernet controller: Apple's GMAC, which every Mac OS X drives
    var adapter = "sungem"
    var forwards: [Forward] = []

    enum CodingKeys: String, CodingKey { case mode, adapter, forwards }

    init() {}

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        mode = (try? c.decodeIfPresent(Mode.self, forKey: .mode)) ?? .nat
        adapter = try c.decodeIfPresent(String.self, forKey: .adapter) ?? "sungem"
        forwards = try c.decodeIfPresent([Forward].self, forKey: .forwards) ?? []
    }
}

/// The VM's screen shared over VNC.
struct Sharing: Codable, Equatable {
    var enabled = false
    var port = 5900
    /// listen for other computers too, not only this Mac
    var remote = false
    /// VeNCrypt with a certificate made for the VM (tls/ in its folder)
    var tls = false
    /// lossy (JPEG) encoding for clients that ask for it
    var jpeg = true

    enum CodingKeys: String, CodingKey { case enabled, port, remote, tls, jpeg }

    init() {}

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        enabled = try c.decodeIfPresent(Bool.self, forKey: .enabled) ?? false
        port = try c.decodeIfPresent(Int.self, forKey: .port) ?? 5900
        remote = try c.decodeIfPresent(Bool.self, forKey: .remote) ?? false
        tls = try c.decodeIfPresent(Bool.self, forKey: .tls) ?? false
        jpeg = try c.decodeIfPresent(Bool.self, forKey: .jpeg) ?? true
    }
}

struct USBPassthrough: Codable, Equatable, Identifiable, Hashable {
    var vendor: Int
    var product: Int
    var name: String

    var id: String { String(format: "%04x:%04x", vendor, product) }
    /// the emulator's device id while connected
    var qdevID: String { String(format: "usb-%04x-%04x", vendor, product) }
}

struct Audio: Codable, Equatable {
    /// the Power Mac's sound chip (Screamer), played through this Mac's
    /// sound output
    var enabled = true
}

enum CPUType: String, Codable, CaseIterable, Identifiable {
    case g3, g4, g4_7447a = "g4-7447a", g5

    var id: String { rawValue }

    var title: String {
        switch self {
        case .g3: return "PowerPC G3 (750)"
        case .g4: return "PowerPC G4 (7400)"
        case .g4_7447a: return "PowerPC G4 (7447A)"
        case .g5: return "PowerPC G5 (970)"
        }
    }

    var shortTitle: String {
        switch self {
        case .g3: return "G3"
        case .g4: return "G4"
        case .g4_7447a: return "G4 (7447A)"
        case .g5: return "G5"
        }
    }

    /// real G3 Macs had one processor
    var maxCores: Int { self == .g3 ? 1 : 4 }
}

enum GraphicsCard: String, Codable, CaseIterable, Identifiable {
    case radeon9600, radeon7000, rage128, vga

    var id: String { rawValue }

    var title: String {
        switch self {
        case .radeon9600: return "ATI Radeon 9600"
        case .radeon7000: return "ATI Radeon 7000"
        case .rage128: return "ATI Rage 128 Pro"
        case .vga: return "Standard VGA"
        }
    }

    func features(for guest: GuestOS) -> String {
        switch self {
        case .radeon9600:
            if guest.rank >= GuestOS.tiger.rank {
                return "Quartz Extreme, Core Image and OpenGL"
            }
            return "Quartz Extreme and OpenGL"
        case .radeon7000:
            return guest.rank >= GuestOS.jaguar.rank
                ? "Quartz Extreme and OpenGL" : "OpenGL"
        case .rage128:
            return guest == .macOS9 ? "Display only (acceleration not working yet)"
                                    : "2D acceleration and OpenGL"
        case .vga:
            return "No acceleration"
        }
    }
}

enum InputMode: String, Codable, CaseIterable, Identifiable {
    case auto, usb, adb
    var id: String { rawValue }
    var title: String {
        switch self {
        case .auto: return "Automatic"
        case .usb: return "USB keyboard and tablet"
        case .adb: return "ADB keyboard and mouse"
        }
    }
}

enum BootDevice: String, Codable, CaseIterable, Identifiable {
    case disk, cdrom
    var id: String { rawValue }
    var title: String { self == .disk ? "Hard Disk" : "CD/DVD" }
}
