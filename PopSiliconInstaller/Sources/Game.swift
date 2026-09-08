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
    /// A resource the installed bundle must still carry, relative to
    /// Contents/Resources.  The Peggle-era titles pack everything into
    /// main.pak; Bejeweled 2 predates that and ships loose folders.
    let resourceMarker: String

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
        imageFileName: "Peggle.image",
        resourceMarker: "main.pak"
    )

    static let nights = Game(
        id: "com.popcap.pegglenights",
        displayName: "Peggle Nights",
        steamFolder: "Peggle Nights",
        steamAppName: "Peggle Nights.app",
        executableName: "Peggle Nights",
        outputAppName: "PeggleNights.app",
        imageFileName: "PeggleNights.image",
        resourceMarker: "main.pak"
    )

    static let bejeweled3 = Game(
        id: "com.popcap.Bejeweled3",
        displayName: "Bejeweled 3",
        steamFolder: "Bejeweled 3",
        steamAppName: "Bejeweled 3.app",
        executableName: "Bejeweled3",
        outputAppName: "Bejeweled3.app",
        imageFileName: "Bejeweled3.image",
        resourceMarker: "main.pak"
    )

    static let bejeweled2 = Game(
        id: "com.popcap.bejeweled2.app",
        displayName: "Bejeweled 2 Deluxe",
        steamFolder: "Bejeweled 2 Deluxe",
        steamAppName: "Bejeweled 2 Deluxe.app",
        executableName: "Bejeweled 2",
        outputAppName: "Bejeweled2.app",
        imageFileName: "Bejeweled2.image",
        resourceMarker: "properties/resources.xml"
    )

    static let chuzzle = Game(
        id: "com.raptisoft.Chuzzle",
        displayName: "Chuzzle Deluxe",
        steamFolder: "Chuzzle Deluxe",
        steamAppName: "Chuzzle Deluxe.app",
        executableName: "Chuzzle",
        outputAppName: "Chuzzle.app",
        imageFileName: "Chuzzle.image",
        resourceMarker: "Data/gamedata.cfg"
    )

    static let plantsVsZombies = Game(
        id: "com.popcap.plantsvszombies",
        displayName: "Plants vs. Zombies",
        steamFolder: "Plants Vs Zombies",
        steamAppName: "Plants vs. Zombies.app",
        executableName: "PlantsvsZombies",
        outputAppName: "PlantsVsZombies.app",
        imageFileName: "PlantsVsZombies.image",
        resourceMarker: "main.pak"
    )

    static let zuma = Game(
        id: "com.popcap.zuma.app",
        displayName: "Zuma Deluxe",
        steamFolder: "Zuma Deluxe",
        steamAppName: "Zuma Deluxe.app",
        executableName: "Zuma",
        outputAppName: "Zuma.app",
        imageFileName: "Zuma.image",
        resourceMarker: "properties/resources.xml"
    )

    static let all: [Game] = [.deluxe, .nights, .bejeweled3, .bejeweled2, .chuzzle,
                              .plantsVsZombies, .zuma]

    /// The game whose original bundle identifier matches this dropped app.
    static func matching(bundleIdentifier: String?) -> Game? {
        guard let bundleIdentifier else { return nil }
        return all.first { $0.id == bundleIdentifier }
    }
}
