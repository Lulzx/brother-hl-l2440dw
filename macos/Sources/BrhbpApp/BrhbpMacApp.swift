import SwiftUI

@main
struct BrhbpMacApp: App {
    @State private var model = PrintModel()

    var body: some Scene {
        WindowGroup {
            ContentView(model: model)
                .task { model.startPolling() }
        }
        .windowStyle(.hiddenTitleBar)
        .windowResizability(.contentSize)
        .defaultSize(width: 980, height: 660)
        .commands {
            CommandGroup(replacing: .newItem) {}
            CommandGroup(after: .newItem) {
                Button("Open Document…") { NotificationCenter.default.post(name: .openDocument, object: nil) }
                    .keyboardShortcut("o")
            }
        }
    }
}

extension Notification.Name {
    static let openDocument = Notification.Name("brhbp.openDocument")
}
