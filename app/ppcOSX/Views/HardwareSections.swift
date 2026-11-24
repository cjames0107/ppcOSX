import SwiftUI

/// A value set with a slider or typed in, clamped to its range.
struct NumberField: View {
    let title: String
    @Binding var value: Int
    let range: ClosedRange<Int>
    var step = 1
    let unit: String
    var slider = true

    private var clamped: Binding<Int> {
        Binding(get: { value },
                set: { value = min(max($0, range.lowerBound), range.upperBound) })
    }

    var body: some View {
        LabeledContent(title) {
            HStack(spacing: 8) {
                if slider && range.upperBound > range.lowerBound {
                    Slider(value: Binding(get: { Double(value) },
                                          set: { clamped.wrappedValue = Int(($0 / Double(step)).rounded()) * step }),
                           in: Double(range.lowerBound)...Double(range.upperBound))
                        .frame(minWidth: 140)
                }
                TextField(title, value: clamped, format: .number.grouping(.never))
                    .labelsHidden()
                    .multilineTextAlignment(.trailing)
                    .frame(width: 64)
                Stepper(title, value: clamped, in: range, step: step)
                    .labelsHidden()
                Text(unit)
                    .foregroundStyle(.secondary)
                    .frame(minWidth: 26, alignment: .leading)
            }
        }
        .onAppear { clamped.wrappedValue = value }
    }
}

/// Processor, cores and memory.
struct SystemSection: View {
    @Binding var config: VMConfig

    var body: some View {
        Section {
            Picker("Processor", selection: $config.cpu) {
                ForEach(CPUType.allCases) { cpu in
                    Text(cpu.title).tag(cpu)
                        .disabled(!config.guest.cpus.contains(cpu))
                }
            }
            .onChange(of: config.cpu) { _, cpu in
                config.cores = min(config.cores, cpu.maxCores)
            }
            NumberField(title: "Processor cores", value: $config.cores,
                        range: 1...config.cpu.maxCores,
                        unit: config.cores == 1 ? "core" : "cores", slider: false)
                .disabled(config.cpu.maxCores == 1)
            NumberField(title: "Memory", value: $config.memory,
                        range: 64...MemoryLimits.max, step: 64, unit: "MB")
        } header: {
            Text("Processor and Memory")
        } footer: {
            VStack(alignment: .leading, spacing: 4) {
                if !config.guest.cpus.contains(config.cpu) {
                    warning("\(config.guest.shortTitle) does not run on a \(config.cpu.shortTitle).")
                }
                if config.memory > config.guest.maxMemory {
                    warning("\(config.guest.shortTitle) may not start with more than \(MemoryLimits.format(config.guest.maxMemory)).")
                } else if config.memory < config.guest.minMemory {
                    warning("\(config.guest.shortTitle) needs at least \(MemoryLimits.format(config.guest.minMemory)).")
                }
            }
        }
    }

    private func warning(_ text: String) -> some View {
        Label(text, systemImage: "exclamationmark.triangle.fill")
            .foregroundStyle(.orange)
    }
}

enum MemoryLimits {
    /// QEMU's Power Mac (mac99) takes at most 2 GB, whatever the processor
    static let machineMax = 2048

    /// MiB: the machine's limit, or this Mac's memory if that is less
    static var max: Int { min(machineMax, Toolkit.hostMemoryMB) }

    static func format(_ mb: Int) -> String {
        mb >= 1024 && mb % 512 == 0
            ? "\(Double(mb) / 1024) GB".replacingOccurrences(of: ".0 ", with: " ")
            : "\(mb) MB"
    }
}

struct DisplaySection: View {
    @Binding var config: VMConfig

    static let resolutions = ["800x600x32", "1024x768x32", "1152x870x32",
                              "1280x1024x32", "1440x900x32", "1600x1200x32"]

    var body: some View {
        Section {
            Picker("Graphics card", selection: $config.graphics) {
                ForEach(GraphicsCard.allCases) { card in
                    HStack {
                        Text(card.title)
                        if card == config.guest.recommendedGraphics {
                            Text("Recommended").foregroundStyle(.secondary)
                        }
                    }
                    .tag(card)
                }
            }
            LabeledContent("Acceleration", value: config.graphics.features(for: config.guest))
            Picker("Startup resolution", selection: $config.resolution) {
                ForEach(Self.resolutions, id: \.self) { r in
                    Text(r.replacingOccurrences(of: "x32", with: "")
                          .replacingOccurrences(of: "x", with: " \u{00d7} ")).tag(r)
                }
            }
        } header: {
            Text("Display")
        } footer: {
            if !config.guest.graphics.contains(config.graphics) {
                Label("\(config.guest.shortTitle) has no driver for the \(config.graphics.title).",
                      systemImage: "exclamationmark.triangle.fill")
                    .foregroundStyle(.orange)
            }
        }
    }
}

struct NetworkSection: View {
    @Binding var network: Network

    var body: some View {
        Section {
            Picker("Network", selection: $network.mode) {
                ForEach(Network.Mode.allCases) { Text($0.title).tag($0) }
            }
            LabeledContent("Adapter", value: "Apple GMAC Ethernet")
        } header: {
            Text("Network")
        }
        if network.mode == .nat {
            Section {
                ForEach($network.forwards) { $f in
                    HStack {
                        Picker("", selection: $f.proto) {
                            Text("TCP").tag("tcp")
                            Text("UDP").tag("udp")
                        }
                        .labelsHidden()
                        .frame(width: 80)
                        TextField("This Mac", value: $f.host, format: .number.grouping(.never))
                        Image(systemName: "arrow.right").foregroundStyle(.secondary)
                        TextField("Guest", value: $f.guest, format: .number.grouping(.never))
                        Button(role: .destructive) {
                            network.forwards.removeAll { $0.id == f.id }
                        } label: {
                            Image(systemName: "minus.circle")
                        }
                        .buttonStyle(.borderless)
                    }
                }
                HStack {
                    Button("Add Port Forward") {
                        network.forwards.append(.init(host: 2222, guest: 22))
                    }
                    Spacer()
                }
            } header: {
                Text("Port Forwarding")
            }
        }
    }
}

struct SoundSection: View {
    @Binding var audio: Audio

    var body: some View {
        Section {
            Toggle("Sound", isOn: $audio.enabled)
        } header: {
            Text("Sound")
        } footer: {
            Text("The Power Mac's sound chip, played through this Mac's sound output. Mac OS X 10.0 through 10.4 have its driver.")
                .font(.footnote)
                .foregroundStyle(.secondary)
        }
    }
}

struct InputSection: View {
    @Binding var config: VMConfig

    var body: some View {
        Section {
            Picker("Keyboard and mouse", selection: $config.input) {
                ForEach(InputMode.allCases) { Text($0.title).tag($0) }
            }
        } header: {
            Text("Input")
        }
    }
}

/// The screen shared over VNC.
struct SharingSection: View {
    @Binding var sharing: Sharing
    /// kept in the keychain; VNC passwords are at most 8 characters
    @Binding var password: String

    var body: some View {
        Section {
            Toggle("Share the screen (VNC)", isOn: $sharing.enabled)
            if sharing.enabled {
                NumberField(title: "Port", value: $sharing.port, range: 5900...5999,
                            unit: "", slider: false)
                Toggle("Allow other computers to connect", isOn: $sharing.remote)
                SecureField("Password", text: Binding(get: { password },
                                                      set: { password = String($0.prefix(8)) }))
                Toggle("Encrypt with TLS", isOn: $sharing.tls)
                Toggle("JPEG compression", isOn: $sharing.jpeg)
                LabeledContent("Address", value: "vnc://\(sharing.remote ? ProcessInfo.processInfo.hostName : "127.0.0.1"):\(sharing.port)")
                    .textSelection(.enabled)
            }
        } header: {
            Text("Screen Sharing")
        } footer: {
            if sharing.enabled && password.isEmpty {
                Label("Set a password: without one, no one can connect.",
                      systemImage: "exclamationmark.triangle.fill")
                    .foregroundStyle(.orange)
            }
        }
    }
}

/// USB devices of this Mac handed to the guest when it starts.
struct USBSection: View {
    @Binding var usb: [USBPassthrough]
    @State private var host: [USBPassthrough] = []

    var body: some View {
        Section {
            ForEach(rows) { d in
                Toggle(isOn: Binding(get: { usb.contains(d) },
                                     set: { on in
                                         if on { usb.append(d) } else { usb.removeAll { $0 == d } }
                                     })) {
                    HStack {
                        Text(d.name)
                        if !host.contains(d) {
                            Text("Not connected").foregroundStyle(.secondary)
                        }
                    }
                }
            }
            HStack {
                Button("Refresh") { host = HostUSB.devices() }
                Spacer()
            }
        } header: {
            Text("USB Devices of This Mac")
        }
        .onAppear { host = HostUSB.devices() }
    }

    /// this Mac's devices, then chosen ones not plugged in now
    private var rows: [USBPassthrough] {
        host + usb.filter { !host.contains($0) }
    }
}
