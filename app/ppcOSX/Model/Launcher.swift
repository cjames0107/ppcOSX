import Foundation

/// Builds a VM's QEMU command line, as `ppcosx run --vm` does (load_vm and
/// build_command there): the same tables (Resources/scripts/
/// machine-data.json, written by `ppcosx machine-data`) and the same Open
/// Firmware Forth generators. tools/check-app-command.sh checks that both
/// produce the same command.
enum Launcher {
    struct MachineData: Decodable {
        struct Profile: Decodable { let binary: String; let cpu: String }
        struct Card: Decodable { let device: String; let env: [String: String] }
        let profiles: [String: Profile]
        let cards: [String: Card]
    }

    static let machineData: MachineData? = {
        guard let url = Toolkit.scripts?.appendingPathComponent("machine-data.json"),
              let data = try? Data(contentsOf: url) else { return nil }
        return try? JSONDecoder().decode(MachineData.self, from: data)
    }()

    static let legacyGuests: Set<String> = ["10.0", "10.1", "10.2", "10.3", "10.4", "10.5"]

    struct Command {
        var executable: URL
        var arguments: [String]
    }

    /// display: QEMU's ppcosx display socket, for the app's own window;
    /// nil for QEMU's own Cocoa window (as `ppcosx run` without
    /// --display-socket)
    /// nvramrc's start when there is sound: evaluate the ppcosx-sound variable
    static let soundLoader = "\" ppcosx-sound\" \" /options\" find-package drop "
        + "get-package-property 0= if evaluate then"

    static func command(folder: URL, config c: VMConfig, socket: String,
                        display: String? = nil) throws -> Command {
        guard let data = machineData, let bin = Toolkit.emulatorBin,
              let scripts = Toolkit.scripts else {
            throw ToolError(message: "The emulator is missing from the application.")
        }
        guard let profile = data.profiles[c.cpu.rawValue] else {
            throw ToolError(message: "Unknown processor \(c.cpu.rawValue).")
        }
        let guest: String? = legacyGuests.contains(c.guest.rawValue) ? c.guest.rawValue : nil
        let smp = max(1, min(4, c.cores))
        let card = data.cards[c.graphics.rawValue]
        let legacyInput = c.input == .adb || (c.input == .auto &&
            (c.guest == .cheetah || c.guest == .puma || c.guest.rawValue.hasPrefix("9")))

        var binaryName = profile.binary
        var smpArgs: [String] = []
        var forth: [String] = []
        if let card {
            var env = card.env
            if profile.cpu.hasPrefix("970") { env["ATI_G5"] = "1" }
            forth.append(try runScript(scripts.appendingPathComponent("ati-agp-nvramrc.sh"),
                                       args: [], env: env))
        }
        if smp > 1 {
            // one host thread per CPU: QEMU's 64-bit binary only
            binaryName = "qemu-system-ppc64"
            forth.append(try runScript(scripts.appendingPathComponent("smp-nvramrc.sh"),
                                       args: ["\(smp)"], env: [:]))
            smpArgs = ["-smp", "\(smp)", "-accel", "tcg,thread=multi"]
        }
        var soundEnv: [String] = []
        if c.audio.enabled {
            // The sound chip's Open Firmware node, in a variable of its own
            // that nvramrc evaluates first (the Rage 128's Forth ends by
            // making the card the console): Mac OS X 10.0 does not start
            // with an nvramrc much over 2500 bytes.
            forth.insert(soundLoader, at: 0)
            soundEnv = ["-prom-env", "ppcosx-sound=" +
                        (try runScript(scripts.appendingPathComponent("sound-nvramrc.sh"),
                                       args: [],
                                       env: profile.cpu.hasPrefix("970") ? ["SOUND_G5": "1"] : [:]))]
        }
        if !forth.isEmpty {
            smpArgs += ["-prom-env", "use-nvramrc?=true",
                        "-prom-env", "nvramrc=" + forth.joined(separator: " ")] + soundEnv
        }
        let binary = bin.appendingPathComponent(binaryName)

        var cmd: [String] = [
            "-M", "mac99,via=\(legacyInput ? "cuda" : "pmu")"
                + (c.audio.enabled ? ",audiodev=snd0" : ""),
            "-cpu", profile.cpu,
            "-m", "\(c.memory)",
        ] + smpArgs + [
            "-g", c.resolution,
            "-prom-env", "auto-boot?=true",
            "-prom-env", "vga-ndrv?=true",
            "-boot", c.boot == .cdrom ? "d" : "c",
        ]
        if c.network.mode == .nat {
            cmd += ["-netdev", "user,id=net0" + c.network.forwards.map {
                        ",hostfwd=\($0.proto):127.0.0.1:\($0.host)-:\($0.guest)"
                    }.joined(),
                    "-device", "\(c.network.adapter),netdev=net0"]
        } else {
            cmd += ["-nic", "none"]
        }

        var drives: [Drive] = []
        for (n, d0) in c.drives.enumerated() {
            var d = d0
            if d.id.isEmpty { d.id = "\(d.type.rawValue)\(n)" }
            if d.isRemote {
                // opened by QEMU itself (ssh://, https://)
            } else if let url = d.url(in: folder) {
                d.file = url.path
                if !FileManager.default.fileExists(atPath: url.path) {
                    if d.type == .cdrom {
                        d.file = ""
                    } else {
                        throw ToolError(message: "\(url.path): no such file")
                    }
                }
            } else if d.type != .cdrom {
                continue
            }
            drives.append(d)
        }
        if !drives.contains(where: { $0.type == .cdrom }) {
            drives.append(Drive(id: "cdrom0", type: .cdrom, file: ""))
        }
        var diskIndex = 0, cdIndex = 2
        for d in drives {
            let ro = d.readOnly ? ",readonly=on" : ""
            let fmt = d.file.hasSuffix(".qcow2") ? "qcow2" : "raw"
            switch d.type {
            case .cdrom:
                let media = d.file.isEmpty ? "" : ",file=\(d.file),format=raw"
                cmd += ["-drive", "id=\(d.id),if=ide,index=\(cdIndex),media=cdrom\(media)"]
                cdIndex += 1
            case .usb:
                cmd += ["-drive", "id=\(d.id),if=none,file=\(d.file),format=\(fmt)\(ro)",
                        "-device", "usb-storage,drive=\(d.id)"]
            case .disk:
                cmd += ["-drive", "id=\(d.id),if=ide,index=\(diskIndex),file=\(d.file),format=\(fmt),media=disk\(ro)"]
                diskIndex += 1
            }
        }

        if !legacyInput {
            var tablet: String? = "usb-tablet"
            if guest == "10.5" { tablet! += ",abs-margin=2458" }
            if guest == "10.0" || guest == "10.1" || guest == "10.2" { tablet = nil }
            if let tablet { cmd += ["-device", tablet] }
        }
        if let card {
            cmd += ["-vga", "none", "-device",
                    card.device + ",agp=on,bus=agp,addr=0x10,id=ati0"]
        }
        for u in c.usb {
            cmd += ["-device", String(format: "usb-host,id=usb-%04x-%04x,vendorid=0x%04x,productid=0x%04x",
                                      u.vendor, u.product, u.vendor, u.product)]
        }
        if c.sharing.enabled {
            var opts = ",password=on" + (c.sharing.jpeg ? ",lossy=on" : "")
            if c.sharing.tls {
                cmd += ["-object", "tls-creds-x509,id=vnctls,dir=" +
                        folder.appendingPathComponent("tls").path +
                        ",endpoint=server,verify-peer=off"]
                opts += ",tls-creds=vnctls"
            }
            let addr = c.sharing.remote ? "0.0.0.0" : "127.0.0.1"
            cmd += ["-vnc", "\(addr):\(c.sharing.port - 5900)\(opts)"]
        }
        if c.audio.enabled {
            cmd += ["-audiodev", "coreaudio,id=snd0"]
        }
        cmd += ["-qmp", "unix:\(socket),server,nowait"]
        cmd += ["-pidfile", folder.appendingPathComponent("qemu.pid").path]
        cmd += ["-name", c.name]
        if let display {
            cmd += ["-display", "ppcosx,socket=\(display)"]
        } else {
            cmd += ["-display", card != nil ? "cocoa,show-cursor=off" : "cocoa"]
        }
        return Command(executable: binary, arguments: cmd)
    }

    private static func runScript(_ script: URL, args: [String],
                                  env: [String: String]) throws -> String {
        let p = Process()
        let out = Pipe()
        p.executableURL = URL(fileURLWithPath: "/bin/sh")
        p.arguments = [script.path] + args
        p.environment = ProcessInfo.processInfo.environment.merging(env) { $1 }
        p.standardOutput = out
        try p.run()
        let data = out.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        guard p.terminationStatus == 0 else {
            throw ToolError(message: "\(script.lastPathComponent) failed")
        }
        return String(decoding: data, as: UTF8.self)
            .trimmingCharacters(in: .whitespacesAndNewlines)
    }
}
