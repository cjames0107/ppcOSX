import SwiftUI
import UniformTypeIdentifiers

struct LibraryView: View {
    @EnvironmentObject var library: VMLibrary
    @State private var selection: UUID?
    @State private var showingWizard = false
    @State private var editing: VirtualMachine?
    @State private var deleting: VirtualMachine?
    @State private var importing = false

    private var selected: VirtualMachine? {
        library.machines.first { $0.id == selection }
    }

    var body: some View {
        NavigationSplitView {
            List(selection: $selection) {
                ForEach(library.machines) { vm in
                    VMRow(vm: vm)
                        .tag(vm.id)
                        .contextMenu { contextMenu(for: vm) }
                        .swipeActions(edge: .trailing, allowsFullSwipe: true) {
                            if !vm.state.isActive {
                                Button(role: .destructive) {
                                    deleting = vm
                                } label: {
                                    Label("Move to Trash", systemImage: "trash")
                                }
                                .tint(.red)
                            }
                        }
                }
                .onMove { library.move(from: $0, to: $1) }
            }
            .navigationSplitViewColumnWidth(min: 220, ideal: 260)
            .overlay {
                if library.machines.isEmpty {
                    ContentUnavailableView {
                        Label("No Virtual Machines", systemImage: "desktopcomputer")
                    } description: {
                        Text("Create one to install Mac OS X.")
                    } actions: {
                        Button("New Virtual Machine\u{2026}") { showingWizard = true }
                    }
                }
            }
            .toolbar {
                ToolbarItem {
                    Button {
                        showingWizard = true
                    } label: {
                        Label("New Virtual Machine", systemImage: "plus")
                    }
                    .help("Create a virtual machine")
                }
            }
        } detail: {
            if let vm = selected {
                VMDetailView(vm: vm, edit: { editing = vm }, delete: { deleting = vm })
                    .id(vm.id)
            } else {
                ContentUnavailableView("No Virtual Machine Selected",
                                       systemImage: "sidebar.left",
                                       description: Text("Choose a virtual machine in the sidebar."))
            }
        }
        .navigationTitle("ppcOSX")
        .sheet(isPresented: $showingWizard) {
            NewVMWizard(created: { vm in selection = vm.id },
                        addExisting: {
                            // after the wizard's sheet has gone
                            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { importing = true }
                        })
                .environmentObject(library)
        }
        .sheet(item: $editing) { vm in
            VMSettingsView(vm: vm)
        }
        .confirmationDialog(deleting.map { "Move \u{201c}\($0.config.name)\u{201d} to the Trash?" } ?? "",
                            isPresented: Binding(get: { deleting != nil },
                                                 set: { if !$0 { deleting = nil } }),
                            titleVisibility: .visible) {
            Button("Move to Trash", role: .destructive) {
                if let vm = deleting {
                    do {
                        try library.delete(vm)
                        selection = library.machines.first?.id
                    } catch {
                        library.error = error.localizedDescription
                    }
                }
                deleting = nil
            }
        } message: {
            Text("Its folder, with its disk images, goes to the Trash.")
        }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.folder]) { result in
            if case .success(let url) = result {
                do {
                    selection = try library.importFolder(url).id
                } catch {
                    library.error = "That folder has no readable config.json: \(error.localizedDescription)"
                }
            }
        }
        .alert("ppcOSX", isPresented: Binding(get: { library.error != nil },
                                              set: { if !$0 { library.error = nil } })) {
            Button("OK") { library.error = nil }
        } message: {
            Text(library.error ?? "")
        }
        .onReceive(NotificationCenter.default.publisher(for: .newVirtualMachine)) { _ in
            showingWizard = true
        }
        .onReceive(NotificationCenter.default.publisher(for: .addExistingVirtualMachine)) { _ in
            importing = true
        }
        .onAppear {
            library.reload()
            if selection == nil { selection = library.machines.first?.id }
            VMFocus.shared.selected = selected
        }
        .onChange(of: selection) { _, _ in VMFocus.shared.selected = selected }
    }

    @ViewBuilder
    private func contextMenu(for vm: VirtualMachine) -> some View {
        if vm.state.isActive {
            Button("Show Window") { vm.showWindow() }
            Button("Shut Down") { vm.shutDown() }
        } else {
            Button("Start") { vm.start() }
        }
        Button("Settings\u{2026}") { editing = vm }
        Divider()
        Button("Show in Finder") {
            NSWorkspace.shared.activateFileViewerSelecting([vm.folder])
        }
        Divider()
        Button("Move to Trash\u{2026}", role: .destructive) { deleting = vm }
            .disabled(vm.state.isActive)
    }
}

struct VMRow: View {
    @ObservedObject var vm: VirtualMachine

    var body: some View {
        HStack(spacing: 10) {
            GuestIcon(guest: vm.config.guest, size: 32)
            VStack(alignment: .leading, spacing: 2) {
                Text(vm.config.name).fontWeight(.medium)
                HStack(spacing: 4) {
                    if vm.state.isActive {
                        Circle().fill(vm.state == .paused ? .orange : .green)
                            .frame(width: 6, height: 6)
                    }
                    Text(vm.state.title)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
        }
        .padding(.vertical, 3)
    }
}

struct GuestIcon: View {
    let guest: GuestOS
    var size: CGFloat = 48

    var body: some View {
        RoundedRectangle(cornerRadius: size * 0.22, style: .continuous)
            .fill(guest.tint.gradient)
            .frame(width: size, height: size)
            .overlay {
                Image(systemName: guest.symbol)
                    .font(.system(size: size * 0.5, weight: .medium))
                    .foregroundStyle(.white)
            }
    }
}
