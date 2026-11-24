import SwiftUI

@main
struct PPCOSXApp: App {
    @StateObject private var library = VMLibrary()
    @ObservedObject private var focus = VMFocus.shared
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate

    init() {
        // tools/check-app-command.sh: print a VM's command line and exit
        let args = CommandLine.arguments
        if let i = args.firstIndex(of: "--print-command"), i + 1 < args.count {
            PPCOSXApp.printCommand(folder: URL(fileURLWithPath: args[i + 1]))
        }
    }

    static func printCommand(folder: URL) -> Never {
        do {
            let data = try Data(contentsOf: folder.appendingPathComponent("config.json"))
            let decoder = JSONDecoder()
            decoder.dateDecodingStrategy = .iso8601
            let config = try decoder.decode(VMConfig.self, from: data)
            let cmd = try Launcher.command(folder: folder, config: config,
                                           socket: folder.appendingPathComponent("qmp.sock").path,
                                           display: folder.appendingPathComponent("display.sock").path)
            let json = try JSONSerialization.data(withJSONObject:
                [cmd.executable.lastPathComponent] + cmd.arguments)
            FileHandle.standardOutput.write(json + Data("\n".utf8))
            exit(0)
        } catch {
            FileHandle.standardError.write(Data("\(error.localizedDescription)\n".utf8))
            exit(1)
        }
    }

    var body: some Scene {
        Window("ppcOSX", id: "library") {
            LibraryView()
                .environmentObject(library)
                .frame(minWidth: 820, minHeight: 520)
                .onAppear { appDelegate.library = library }
        }
        .commands {
            VMCommands()
            CommandGroup(replacing: .newItem) {
                Button("New Virtual Machine\u{2026}") {
                    NotificationCenter.default.post(name: .newVirtualMachine, object: nil)
                }
                .keyboardShortcut("n")
                // a VM window in front gets these shortcuts (its New, Open...)
                .disabled(focus.machine != nil)
                Button("Add Existing Virtual Machine\u{2026}") {
                    NotificationCenter.default.post(name: .addExistingVirtualMachine, object: nil)
                }
                .keyboardShortcut("o")
                .disabled(focus.machine != nil)
                Divider()
                Button("Show Library in Finder") {
                    NSWorkspace.shared.open(library.root)
                }
                Button("Refresh Library") { library.reload() }
                    .keyboardShortcut("r")
                    .disabled(focus.machine != nil)
            }
        }

        Settings {
            PreferencesView()
                .environmentObject(library)
        }
    }
}

extension Notification.Name {
    static let newVirtualMachine = Notification.Name("newVirtualMachine")
    static let addExistingVirtualMachine = Notification.Name("addExistingVirtualMachine")
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    weak var library: VMLibrary?

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    /// VM windows are one per VM, never tabs
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSWindow.allowsAutomaticWindowTabbing = false
    }

    /// Running VMs: shut them down first (each guest may ask to confirm,
    /// in its window), or leave them running without windows; reopening
    /// ppcOSX finds them again.
    @MainActor
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let library, library.anyRunning else { return .terminateNow }
        let running = library.machines.filter { $0.state.isActive }
        let alert = NSAlert()
        alert.messageText = running.count == 1
            ? "\u{201C}\(running[0].config.name)\u{201D} is running"
            : "\(running.count) virtual machines are running"
        alert.informativeText = "Shut down before quitting, or leave running without a window; ppcOSX shows it again when it next opens."
        alert.addButton(withTitle: "Shut Down")
        alert.addButton(withTitle: "Cancel")
        alert.addButton(withTitle: "Leave Running")
        switch alert.runModal() {
        case .alertFirstButtonReturn:
            for vm in running {
                vm.showWindow()
                vm.shutDown()
            }
            waitForShutdown(library, since: Date())
            return .terminateLater
        case .alertThirdButtonReturn:
            return .terminateNow
        default:
            return .terminateCancel
        }
    }

    /// Quit once every VM stopped; give up after two minutes (a guest may
    /// be waiting for its shutdown to be confirmed, or have refused).
    @MainActor
    private func waitForShutdown(_ library: VMLibrary, since start: Date) {
        if !library.anyRunning {
            NSApp.reply(toApplicationShouldTerminate: true)
        } else if Date().timeIntervalSince(start) > 120 {
            NSApp.reply(toApplicationShouldTerminate: false)
        } else {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
                self?.waitForShutdown(library, since: start)
            }
        }
    }
}
