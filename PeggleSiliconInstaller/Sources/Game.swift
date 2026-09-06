import Foundation

/// A supported PopCap title.  The installer builds and installs one game at a
/// time, chosen from the game selector; the native loader auto-detects which
/// game an image is, so a single loader binary serves both.
struct Game: Identifiable, Hashable {
    let id: String                 // CFBundleIdentifier of the original game
    let displayName: String        // "Peggle Deluxe"
    let steamFolder: String        // steamapps/common/<steamFolder>/
    let steamAppName: String       // "<name>.app" inside that folder
    let executableName: String     // Contents/MacOS/<executableName> Steam launches
    let outputAppName: String      // standalone export bundle name
    let imageFileName: String      // SharedSupport/<imageFileName> a PeggleSilicon install stores

    /// Steam install location for this game, if the folder/app exist.
    var steamInstallationURL: URL? {
        let installation = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/Steam", isDirectory: true)
            .appendingPathComponent("steamapps/common", isDirectory: true)
            .appendingPathComponent(steamFolder, isDirectory: true)
            .appendingPathComponent(steamAppName, isDirectory: true)
        let executable = installation.appendingPathComponent("Contents/MacOS/\(executableName)")
        guard FileManager.default.fileExists(atPath: installation.path),
              FileManager.default.fileExists(atPath: executable.path) else {
            return nil
        }
        return installation
    }

    static let deluxe = Game(
        id: "com.popcap.peggle",
        displayName: "Peggle Deluxe",
        steamFolder: "Peggle Deluxe",
        steamAppName: "Peggle Deluxe.app",
        executableName: "Peggle",
        outputAppName: "PeggleSilicon.app",
        imageFileName: "Peggle.image"
    )

    static let nights = Game(
        id: "com.popcap.pegglenights",
        displayName: "Peggle Nights",
        steamFolder: "Peggle Nights",
        steamAppName: "Peggle Nights.app",
        executableName: "Peggle Nights",
        outputAppName: "PeggleNights.app",
        imageFileName: "PeggleNights.image"
    )

    static let all: [Game] = [.deluxe, .nights]

    /// The game whose original bundle identifier matches this dropped app.
    static func matching(bundleIdentifier: String?) -> Game? {
        guard let bundleIdentifier else { return nil }
        return all.first { $0.id == bundleIdentifier }
    }
}
