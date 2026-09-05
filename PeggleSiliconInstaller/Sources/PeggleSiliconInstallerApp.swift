import SwiftUI

@main
struct PeggleSiliconInstallerApp: App {
    @StateObject private var model = InstallerModel()

    var body: some Scene {
        WindowGroup("Install PeggleSilicon") {
            ContentView(model: model)
        }
        .windowStyle(.titleBar)
    }
}
