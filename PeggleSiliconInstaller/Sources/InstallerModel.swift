import AppKit
import Combine
import Foundation
import UniformTypeIdentifiers

@MainActor
final class InstallerModel: ObservableObject {
    @Published private(set) var sourceURL: URL?
    @Published private(set) var destinationDirectory: URL?
    @Published private(set) var isBuilding = false
    @Published var isDropTargeted = false
    @Published private(set) var statusMessage = "Drop the game app to begin."
    @Published private(set) var errorMessage: String?
    @Published private(set) var buildOutput: String?
    @Published var isBuildOutputExpanded = false
    @Published private(set) var installationSucceeded = false
    @Published private(set) var successProgress = 0.0
    @Published private(set) var steamInstallationURL: URL?
    @Published private(set) var steamInstallationState: SteamLocator.InstallationState = .unsupported
    @Published private(set) var steamReplacementSucceeded = false
    @Published private(set) var steamBackupURL: URL?
    @Published var showSteamReplacementConfirmation = false
    private let pegHitSoundPlayer = PegHitSoundPlayer()

    init() {
        let steamInstallation = SteamLocator.find()
        steamInstallationURL = steamInstallation
        if let steamInstallation {
            steamInstallationState = SteamLocator.state(of: steamInstallation)
        }
        switch steamInstallationState {
        case .unpatched:
            statusMessage = "Steam installation detected. It can be installed directly."
        case .peggleSilicon:
            statusMessage = "PeggleSilicon is already installed in Steam."
        case .unsupported:
            statusMessage = steamInstallation == nil
                ? "Drop the game app to begin."
                : "Steam installation found, but it is not an unmodified 32-bit copy."
        }
    }

    var destinationURL: URL? {
        destinationDirectory?.appendingPathComponent("PeggleSilicon.app", isDirectory: true)
    }

    var canExport: Bool {
        sourceURL != nil && destinationDirectory != nil && !isBuilding
    }

    var canReplaceSteam: Bool {
        steamInstallationURL != nil
            && steamInstallationState == .unpatched
            && !isBuilding
    }

    func acceptDrop(providers: [NSItemProvider]) -> Bool {
        guard let provider = providers.first(where: {
            $0.hasItemConformingToTypeIdentifier(UTType.fileURL.identifier)
        }) else {
            return false
        }

        provider.loadObject(ofClass: NSURL.self) { [weak self] object, _ in
            let url = (object as? NSURL).map { $0 as URL }
            Task { @MainActor [weak self] in
                self?.setSource(url)
            }
        }
        return true
    }

    func chooseDestination() {
        let panel = NSOpenPanel()
        panel.title = "Choose Export Location"
        panel.message = "PeggleSilicon.app will be created in this folder."
        panel.prompt = "Choose"
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.canCreateDirectories = true
        panel.allowsMultipleSelection = false

        guard panel.runModal() == .OK, let url = panel.url else { return }
        destinationDirectory = url
        errorMessage = nil
        statusMessage = "Ready to export."
    }

    func export() {
        guard let sourceURL, let destinationURL else { return }
        guard let projectRoot = ProjectLocator.find() else {
            errorMessage = "PeggleSilicon project files could not be found next to this app."
            return
        }

        isBuilding = true
        errorMessage = nil
        buildOutput = nil
        installationSucceeded = false
        successProgress = 0
        statusMessage = "Building…"

        Task { [weak self] in
            let result = await BuildRunner.run(
                source: sourceURL,
                destination: destinationURL,
                projectRoot: projectRoot
            )

            guard let self else { return }
            isBuilding = false
            buildOutput = result.output
            if result.succeeded {
                statusMessage = "Installation complete."
                installationSucceeded = true
                pegHitSoundPlayer.play(from: sourceURL, projectRoot: projectRoot)
                successProgress = 0
                Task { @MainActor [weak self] in
                    try? await Task.sleep(nanoseconds: 80_000_000)
                    self?.successProgress = 1
                }
            } else {
                statusMessage = "Export failed."
                errorMessage = "The build script returned exit status \(result.exitStatus)."
            }
        }
    }

    func requestSteamReplacement() {
        guard steamInstallationURL != nil, !isBuilding else { return }
        guard steamInstallationState == .unpatched else {
            errorMessage = "The Steam app is already patched or is not an unmodified 32-bit Peggle installation."
            return
        }
        errorMessage = nil
        showSteamReplacementConfirmation = true
    }

    func confirmSteamReplacement() {
        showSteamReplacementConfirmation = false
        guard let steamInstallationURL else { return }
        guard steamInstallationState == .unpatched else {
            errorMessage = "The Steam app is already patched or is not an unmodified 32-bit Peggle installation."
            return
        }
        guard let projectRoot = ProjectLocator.find() else {
            errorMessage = "PeggleSilicon project files could not be found next to this app."
            return
        }

        isBuilding = true
        errorMessage = nil
        buildOutput = nil
        installationSucceeded = false
        successProgress = 0
        steamReplacementSucceeded = false
        steamBackupURL = steamInstallationURL.appendingPathExtension("bak")
        statusMessage = "Installing into Steam…"

        Task { [weak self] in
            let result = await SteamReplacementRunner.run(
                source: steamInstallationURL,
                target: steamInstallationURL,
                projectRoot: projectRoot
            )

            guard let self else { return }
            isBuilding = false
            buildOutput = result.output
            if result.succeeded {
                statusMessage = "Installation complete."
                steamReplacementSucceeded = true
                installationSucceeded = true
                steamInstallationState = .peggleSilicon
                pegHitSoundPlayer.play(from: steamInstallationURL, projectRoot: projectRoot)
                successProgress = 0
                Task { @MainActor [weak self] in
                    try? await Task.sleep(nanoseconds: 80_000_000)
                    self?.successProgress = 1
                }
            } else {
                statusMessage = "Installation failed."
                errorMessage = "The Steam installation was not replaced."
            }
        }
    }

    private func setSource(_ url: URL?) {
        guard let url else {
            errorMessage = "The dropped item could not be read."
            return
        }

        if url.standardizedFileURL == steamInstallationURL?.standardizedFileURL,
           SteamLocator.state(of: url) != .unpatched {
            errorMessage = "The Steam app is already patched or is not an unmodified 32-bit Peggle installation."
            return
        }

        let executable = url.appendingPathComponent("Contents/MacOS/Peggle")
        guard url.pathExtension.lowercased() == "app",
              FileManager.default.fileExists(atPath: executable.path) else {
            errorMessage = "Drop the original Peggle Deluxe.app bundle."
            return
        }

        sourceURL = url
        errorMessage = nil
        installationSucceeded = false
        successProgress = 0
        steamReplacementSucceeded = false
        statusMessage = "Game app selected. Choose an export location."
    }
}

enum ProjectLocator {
    static func find() -> URL? {
        var candidate = Bundle.main.bundleURL.deletingLastPathComponent()
        for _ in 0..<5 {
            let script = candidate.appendingPathComponent("tools/build.py")
            let nativeMakefile = candidate.appendingPathComponent("native/Makefile")
            if FileManager.default.fileExists(atPath: script.path),
               FileManager.default.fileExists(atPath: nativeMakefile.path) {
                return candidate
            }
            candidate.deleteLastPathComponent()
        }
        return nil
    }
}
