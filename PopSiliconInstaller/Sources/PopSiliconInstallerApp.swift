import SwiftUI

@main
struct PopSiliconInstallerApp: App {
    @StateObject private var model = InstallerModel()

    var body: some Scene {
        WindowGroup("PopSilicon Installer") {
            ContentView(model: model)
        }
        .windowStyle(.titleBar)
    }
}
