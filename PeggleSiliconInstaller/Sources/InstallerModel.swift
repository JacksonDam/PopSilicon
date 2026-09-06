import AppKit
import Combine
import Foundation
import UniformTypeIdentifiers

@MainActor
final class InstallerModel: ObservableObject {
    /// The game the drop box and Steam box currently target.  Defaults to
    /// Peggle Deluxe; the game selector changes it.
    @Published var selectedGame: Game = .deluxe {
        didSet {
            guard selectedGame != oldValue else { return }
            if let sourceURL, SteamLocator.game(of: sourceURL) != selectedGame {
                self.sourceURL = nil
            }
            errorMessage = nil
            installationSucceeded = false
            steamReplacementSucceeded = false
            refreshSteamInstallation()
        }
    }
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
        refreshSteamInstallation()
    }

    var availableGames: [Game] { Game.all }

    var destinationURL: URL? {
        destinationDirectory?.appendingPathComponent(selectedGame.outputAppName, isDirectory: true)
    }

    var canExport: Bool {
        sourceURL != nil && destinationDirectory != nil && !isBuilding
    }

    /// Steam's install can be installed into or repaired.
    private var steamIsInstallable: Bool {
        steamInstallationState == .unpatched || steamInstallationState == .needsRepair
    }

    /// The bundle whose executable becomes the game image of the Steam install.
    /// Steam's own copy is DRM-wrapped, but the build unwraps it, so no separate
    /// download is required.  A dropped copy of the selected game overrides, and
    /// a prior `.bak` backup is the source when repairing an existing install.
    var steamBuildSource: URL? {
        if let sourceURL { return sourceURL }
        guard let steamInstallationURL else { return nil }
        let backup = steamInstallationURL.appendingPathExtension("bak")
        switch steamInstallationState {
        case .unpatched:
            return steamInstallationURL
        case .needsRepair, .peggleSilicon:
            return FileManager.default.fileExists(atPath: backup.path) ? backup : nil
        case .unsupported:
            return nil
        }
    }

    var canReplaceSteam: Bool {
        steamInstallationURL != nil
            && steamIsInstallable
            && steamBuildSource != nil
            && !isBuilding
    }

    var steamActionTitle: String {
        steamInstallationState == .needsRepair
            ? "Repair Steam installation…"
            : "Replace Steam installation…"
    }

    var steamAlertTitle: String {
        steamInstallationState == .needsRepair
            ? "Repair Steam installation?"
            : "Replace Steam installation?"
    }

    var steamAlertMessage: String {
        let drmWrapped = steamBuildSource.map { SteamLocator.sourceNeedsSteam($0, for: selectedGame) } ?? false
        let needsSteam = drmWrapped
            ? "\n\nSteam must be running and signed in to the account that owns "
                + "\(selectedGame.displayName), which is used once to unwrap the game's DRM."
            : ""
        if steamInstallationState == .needsRepair {
            return "PeggleSilicon in Steam will be rebuilt in place; the existing "
                + "\(selectedGame.steamAppName).bak backup is kept." + needsSteam
        }
        return "The original will be renamed to \(selectedGame.steamAppName).bak before "
            + "PeggleSilicon is installed." + needsSteam
    }

    var steamAlertButtonTitle: String {
        steamInstallationState == .needsRepair ? "Repair" : "Replace and Install"
    }

    var dropZoneHint: String {
        if steamInstallationURL != nil, steamIsInstallable {
            return "Optional for Steam; needed only to export a standalone copy to a folder."
        }
        return "The original \(selectedGame.displayName) application is required."
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
        panel.message = "\(selectedGame.outputAppName) will be created in this folder."
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
        guard steamIsInstallable, steamBuildSource != nil else {
            errorMessage = "The Steam app is already patched or is not an unmodified 32-bit \(selectedGame.displayName) installation."
            return
        }
        errorMessage = nil
        showSteamReplacementConfirmation = true
    }

    func confirmSteamReplacement() {
        showSteamReplacementConfirmation = false
        guard let steamInstallationURL, let source = steamBuildSource else { return }
        let game = selectedGame
        guard steamIsInstallable else {
            errorMessage = "The Steam app is already patched or is not an unmodified 32-bit \(game.displayName) installation."
            return
        }
        guard let projectRoot = ProjectLocator.find() else {
            errorMessage = "PeggleSilicon project files could not be found next to this app."
            return
        }

        let repairing = steamInstallationState == .needsRepair
        let backup = steamInstallationURL.appendingPathExtension("bak")
        isBuilding = true
        errorMessage = nil
        buildOutput = nil
        installationSucceeded = false
        successProgress = 0
        steamReplacementSucceeded = false
        steamBackupURL = backup
        statusMessage = repairing ? "Repairing the Steam installation…" : "Installing into Steam…"

        Task { [weak self] in
            let result = await SteamReplacementRunner.run(
                source: source,
                target: steamInstallationURL,
                game: game,
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
                steamBackupURL = FileManager.default.fileExists(atPath: backup.path) ? backup : nil
                pegHitSoundPlayer.play(from: source, projectRoot: projectRoot)
                successProgress = 0
                Task { @MainActor [weak self] in
                    try? await Task.sleep(nanoseconds: 80_000_000)
                    self?.successProgress = 1
                }
            } else {
                statusMessage = "Installation failed."
                errorMessage = repairing
                    ? "The Steam installation was not repaired."
                    : "The Steam installation was not replaced."
            }
        }
    }

    private func refreshSteamInstallation() {
        let installation = SteamLocator.find(selectedGame)
        steamInstallationURL = installation
        steamInstallationState = installation.map { SteamLocator.state(of: $0, game: selectedGame) } ?? .unsupported
        refreshStatusMessage()
    }

    private func refreshStatusMessage() {
        guard steamInstallationURL != nil else {
            statusMessage = "Drop the \(selectedGame.displayName) app to begin."
            return
        }
        switch steamInstallationState {
        case .unpatched:
            statusMessage = "\(selectedGame.displayName) Steam installation detected. It can be installed directly."
        case .needsRepair:
            statusMessage = steamBuildSource != nil
                ? "PeggleSilicon in Steam needs repair. It can be repaired directly."
                : "PeggleSilicon in Steam needs repair, but no backup was found to rebuild from."
        case .peggleSilicon:
            statusMessage = "PeggleSilicon is already installed in \(selectedGame.displayName) on Steam."
        case .unsupported:
            statusMessage = "\(selectedGame.displayName) Steam installation found, but it is not an unmodified 32-bit copy."
        }
    }

    private func setSource(_ url: URL?) {
        guard let url else {
            errorMessage = "The dropped item could not be read."
            return
        }
        let supported = Game.all.map(\.displayName)
        let supportedList = supported.dropLast().joined(separator: ", ") + " or " + supported.last!
        guard url.pathExtension.lowercased() == "app" else {
            errorMessage = "Drop the original \(supportedList) app bundle."
            return
        }
        // Identify which game was dropped; follow the drop by switching the
        // selector so the whole UI targets it.
        guard let droppedGame = SteamLocator.game(of: url) else {
            errorMessage = "This app is not \(supportedList)."
            return
        }
        if droppedGame != selectedGame {
            selectedGame = droppedGame
        }
        guard SteamLocator.isSource(url, for: droppedGame) else {
            errorMessage = "This \(droppedGame.displayName) app does not contain the original 32-bit executable."
            return
        }

        sourceURL = url
        errorMessage = nil
        installationSucceeded = false
        successProgress = 0
        steamReplacementSucceeded = false
        statusMessage = "\(droppedGame.displayName) selected. Choose an export location."
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
