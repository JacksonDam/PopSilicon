import AppKit
import Combine
import Foundation
import UniformTypeIdentifiers

/// Which confirmation the installer's single alert presentation is showing.
/// SwiftUI honours one alert per view hierarchy, and the chooser sits inside
/// the view that already carries one, so a second `.alert` there never
/// presents — the button would appear to do nothing.
enum InstallerAlert: Int, Identifiable {
    case steamReplacement
    case bulkUpdate

    var id: Int { rawValue }
}

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
    /// The product being installed; nil shows the product chooser.
    @Published private(set) var product: Product?
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
    /// How the finished Steam action is described once it has succeeded.
    @Published private(set) var steamCompletionLabel = "installed in Steam"
    @Published private(set) var steamBackupURL: URL?
    @Published var activeAlert: InstallerAlert?
    /// The chooser's "update everything in Steam" pass.
    @Published private(set) var isBulkUpdating = false
    @Published private(set) var bulkUpdateProgress: String?
    @Published private(set) var bulkUpdateSummary: String?

    init() {
        refreshSteamInstallation()
    }

    /// Only the chosen product's games are offered.
    var availableGames: [Game] { product?.games ?? [] }

    /// Name of the compatibility app being installed ("PeggleSilicon" or
    /// "BejeweledSilicon"), for messages.
    var productName: String { product?.displayName ?? "PopSilicon" }

    func selectProduct(_ product: Product) {
        self.product = product
        if product.games.contains(selectedGame) {
            errorMessage = nil
            installationSucceeded = false
            steamReplacementSucceeded = false
            refreshSteamInstallation()
        } else {
            selectedGame = product.games[0]   // didSet resets and refreshes
        }
    }

    func chooseAnotherProduct() {
        guard !isBuilding else { return }
        product = nil
        sourceURL = nil
        errorMessage = nil
        buildOutput = nil
        installationSucceeded = false
        steamReplacementSucceeded = false
    }

    var destinationURL: URL? {
        destinationDirectory?.appendingPathComponent(selectedGame.outputAppName, isDirectory: true)
    }

    var canExport: Bool {
        sourceURL != nil && destinationDirectory != nil && !isBuilding
    }

    /// Steam's install can be installed into, repaired, or updated in place to
    /// this copy of the project.  An install that is already PopSilicon is
    /// rebuilt from its backup, so `steamBuildSource` still decides whether the
    /// action can run.
    private var steamIsInstallable: Bool {
        steamInstallationState != .unsupported
    }

    /// What the Steam action does in the current state: install over Valve's
    /// copy, repair an install whose game image is still encrypted, or update
    /// a good install to the current build.
    private enum SteamAction {
        case replace, repair, update

        var completionLabel: String {
            switch self {
            case .replace: return "installed in Steam"
            case .repair: return "repaired in Steam"
            case .update: return "updated in Steam"
            }
        }
    }

    private var steamAction: SteamAction {
        switch steamInstallationState {
        case .needsRepair: return .repair
        case .peggleSilicon: return .update
        case .unpatched, .unsupported: return .replace
        }
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
        switch steamAction {
        case .repair: return "Repair Steam installation…"
        case .update: return "Update Steam installation…"
        case .replace: return "Replace Steam installation…"
        }
    }

    var steamAlertTitle: String {
        switch steamAction {
        case .repair: return "Repair Steam installation?"
        case .update: return "Update Steam installation?"
        case .replace: return "Replace Steam installation?"
        }
    }

    var steamAlertMessage: String {
        let drmWrapped = steamBuildSource.map { SteamLocator.sourceNeedsSteam($0, for: selectedGame) } ?? false
        let needsSteam = drmWrapped
            ? "\n\nSteam must be running and signed in to the account that owns "
                + "\(selectedGame.displayName), which is used once to unwrap the game's DRM."
            : ""
        switch steamAction {
        case .repair:
            return "\(productName) in Steam will be rebuilt in place; the existing "
                + "\(selectedGame.steamAppName).bak backup is kept." + needsSteam
        case .update:
            return "The \(productName) installed in Steam will be rebuilt from this "
                + "copy of the project and replaced; the existing "
                + "\(selectedGame.steamAppName).bak backup is kept." + needsSteam
        case .replace:
            return "The original will be renamed to \(selectedGame.steamAppName).bak before "
                + "\(productName) is installed." + needsSteam
        }
    }

    var steamAlertButtonTitle: String {
        switch steamAction {
        case .repair: return "Repair"
        case .update: return "Update"
        case .replace: return "Replace and Install"
        }
    }

    /// Every game whose Steam copy already runs PopSilicon and still has the
    /// backup a rebuild needs.  The chooser offers these in one pass, so a new
    /// build reaches all of them without walking the products one at a time.
    var updatableSteamGames: [Game] {
        Game.all.filter { game in
            guard let installation = SteamLocator.find(game) else { return false }
            let state = SteamLocator.state(of: installation, game: game)
            guard state == .peggleSilicon || state == .needsRepair else { return false }
            return FileManager.default.fileExists(
                atPath: installation.appendingPathExtension("bak").path)
        }
    }

    var canBulkUpdateSteam: Bool {
        !isBulkUpdating && !isBuilding && !updatableSteamGames.isEmpty
    }

    var bulkUpdateActionTitle: String {
        isBulkUpdating
            ? "Updating…"
            : "Update All Steam Installations (\(updatableSteamGames.count))"
    }

    var bulkUpdateHeadline: String {
        if isBulkUpdating { return "Updating Steam installations" }
        return bulkUpdateSummary ?? "PopSilicon is installed in Steam"
    }

    var bulkUpdateSymbol: String {
        if isBulkUpdating { return "arrow.triangle.2.circlepath" }
        guard let summary = bulkUpdateSummary else { return "arrow.down.circle" }
        return summary.contains("could not") ? "exclamationmark.triangle" : "checkmark.circle"
    }

    var bulkUpdateDetail: String? {
        if let bulkUpdateProgress { return bulkUpdateProgress }
        let names = updatableSteamGames.map(\.displayName)
        guard !names.isEmpty else { return nil }
        return "Reinstall each one from unmodified backup so the games have the "
            + "newest version of PopSilicon: " + names.joined(separator: ", ") + "."
    }

    var bulkUpdateAlertMessage: String {
        let games = updatableSteamGames
        let drmWrapped = games.filter { game in
            guard let installation = SteamLocator.find(game) else { return false }
            return SteamLocator.sourceNeedsSteam(
                installation.appendingPathExtension("bak"), for: game)
        }
        let plural = games.count == 1 ? "" : "s"
        let needsSteam = drmWrapped.isEmpty
            ? ""
            : "\n\nSteam must be running and signed in: "
                + drmWrapped.map(\.displayName).joined(separator: ", ")
                + (drmWrapped.count == 1 ? " is" : " are") + " DRM-wrapped and "
                + (drmWrapped.count == 1 ? "its" : "their") + " code is unwrapped again."
        return "\(games.count) Steam installation\(plural) will be rebuilt in place; "
            + "each existing backup is kept." + needsSteam
    }

    func requestBulkSteamUpdate() {
        guard canBulkUpdateSteam else { return }
        errorMessage = nil
        bulkUpdateSummary = nil
        activeAlert = .bulkUpdate
    }

    func confirmBulkSteamUpdate() {
        activeAlert = nil
        guard let projectRoot = ProjectLocator.find() else {
            errorMessage = "PopSilicon project files could not be found next to this app."
            return
        }
        let games = updatableSteamGames
        guard !games.isEmpty else { return }
        isBulkUpdating = true
        errorMessage = nil
        bulkUpdateSummary = nil

        Task { [weak self] in
            var updated: [String] = []
            var failed: [String] = []
            for (index, game) in games.enumerated() {
                guard let installation = SteamLocator.find(game) else { continue }
                self?.bulkUpdateProgress =
                    "\(index + 1) of \(games.count): \(game.displayName)…"
                let result = await SteamReplacementRunner.run(
                    source: installation.appendingPathExtension("bak"),
                    target: installation,
                    game: game,
                    projectRoot: projectRoot
                )
                if result.succeeded {
                    updated.append(game.displayName)
                } else {
                    failed.append(game.displayName)
                }
                self?.buildOutput = result.output
            }

            guard let self else { return }
            isBulkUpdating = false
            bulkUpdateProgress = nil
            let plural = updated.count == 1 ? "" : "s"
            bulkUpdateSummary = failed.isEmpty
                ? "Updated \(updated.count) Steam installation\(plural)"
                : "Updated \(updated.count); could not update "
                    + failed.joined(separator: ", ")
            refreshSteamInstallation()
        }
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
            errorMessage = "PopSilicon project files could not be found next to this app."
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
            errorMessage = steamIsInstallable
                ? "No \(selectedGame.steamAppName).bak backup was found to rebuild this installation from."
                : "The Steam app is not an unmodified 32-bit \(selectedGame.displayName) installation."
            return
        }
        errorMessage = nil
        activeAlert = .steamReplacement
    }

    func confirmSteamReplacement() {
        activeAlert = nil
        guard let steamInstallationURL, let source = steamBuildSource else { return }
        let game = selectedGame
        guard steamIsInstallable else {
            errorMessage = "The Steam app is not an unmodified 32-bit \(game.displayName) installation."
            return
        }
        guard let projectRoot = ProjectLocator.find() else {
            errorMessage = "PopSilicon project files could not be found next to this app."
            return
        }

        let action = steamAction
        let backup = steamInstallationURL.appendingPathExtension("bak")
        isBuilding = true
        errorMessage = nil
        buildOutput = nil
        installationSucceeded = false
        successProgress = 0
        steamReplacementSucceeded = false
        steamBackupURL = backup
        switch action {
        case .repair: statusMessage = "Repairing the Steam installation…"
        case .update: statusMessage = "Updating the Steam installation…"
        case .replace: statusMessage = "Installing into Steam…"
        }

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
                steamCompletionLabel = action.completionLabel
                installationSucceeded = true
                steamInstallationState = .peggleSilicon
                steamBackupURL = FileManager.default.fileExists(atPath: backup.path) ? backup : nil
                successProgress = 0
                Task { @MainActor [weak self] in
                    try? await Task.sleep(nanoseconds: 80_000_000)
                    self?.successProgress = 1
                }
            } else {
                statusMessage = "Installation failed."
                switch action {
                case .repair: errorMessage = "The Steam installation was not repaired."
                case .update: errorMessage = "The Steam installation was not updated."
                case .replace: errorMessage = "The Steam installation was not replaced."
                }
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
                ? "\(productName) in Steam needs repair. It can be repaired directly."
                : "\(productName) in Steam needs repair, but no backup was found to rebuild from."
        case .peggleSilicon:
            statusMessage = steamBuildSource != nil
                ? "\(productName) is installed in \(selectedGame.displayName) on Steam. It can be updated to this build."
                : "\(productName) is installed in \(selectedGame.displayName) on Steam, but no backup was found to rebuild from."
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
        // A dropped game also picks its product.
        let droppedProduct = Product.containing(droppedGame)
        if product != droppedProduct {
            product = droppedProduct
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
