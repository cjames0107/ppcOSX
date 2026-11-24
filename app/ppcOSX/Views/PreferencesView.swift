import SwiftUI

struct PreferencesView: View {
    @EnvironmentObject var library: VMLibrary
    @AppStorage(Toolkit.libraryKey) private var libraryPath = ""
    @State private var choosingLibrary = false

    var body: some View {
        Form {
            Section {
                LabeledContent("Virtual machines") {
                    HStack {
                        Text(Toolkit.libraryFolder.path).lineLimit(1).truncationMode(.middle)
                        Button("Change\u{2026}") { choosingLibrary = true }
                        if !libraryPath.isEmpty {
                            Button("Default") { libraryPath = ""; library.reload() }
                        }
                    }
                }
            }
            Section {
                LabeledContent("Emulator") {
                    Label(Toolkit.isComplete ? "Included" : "Missing from the application",
                          systemImage: Toolkit.isComplete ? "checkmark.circle.fill"
                                                          : "exclamationmark.triangle.fill")
                        .foregroundStyle(Toolkit.isComplete ? .green : .orange)
                }
            }
        }
        .formStyle(.grouped)
        .frame(width: 560)
        .fileImporter(isPresented: $choosingLibrary, allowedContentTypes: [.folder]) { r in
            if case .success(let url) = r { libraryPath = url.path; library.reload() }
        }
    }
}
