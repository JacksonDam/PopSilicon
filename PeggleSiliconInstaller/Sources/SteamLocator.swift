import Foundation

enum SteamLocator {
    enum InstallationState: Equatable {
        /// Valve's copy of the game is still in place.
        case unpatched
        /// PeggleSilicon is installed and its game image is loadable.
        case peggleSilicon
        /// PeggleSilicon is installed, but its game image is still Steam's
        /// DRM-encrypted executable, which the loader cannot start (an older
        /// install predating in-installer DRM unwrapping); it can be repaired.
        case needsRepair
        case unsupported
    }

    /// Steam install for the given game, if present.
    static func find(_ game: Game) -> URL? {
        game.steamInstallationURL
    }

    static func state(of installation: URL, game: Game) -> InstallationState {
        let contents = installation.appendingPathComponent("Contents")
        let executable = contents.appendingPathComponent("MacOS/\(game.executableName)")
        let resources = contents.appendingPathComponent("Resources/main.pak")
        let sharedImage = contents.appendingPathComponent("SharedSupport/\(game.imageFileName)")

        guard FileManager.default.fileExists(atPath: executable.path),
              FileManager.default.fileExists(atPath: resources.path) else {
            return .unsupported
        }

        // PeggleSilicon stores the game image separately before replacing the
        // Steam entry point with its native loader.  If that image is still the
        // DRM-encrypted executable, the install predates DRM unwrapping and
        // needs repair; otherwise it is a good install.
        if FileManager.default.fileExists(atPath: sharedImage.path) {
            return MachOInspector.isSteamDRMWrapped(at: sharedImage) ? .needsRepair : .peggleSilicon
        }

        return MachOInspector.isI386Executable(at: executable) ? .unpatched : .unsupported
    }

    /// Whether a bundle is the original 32-bit game for `game` (retail or
    /// Steam's DRM copy — both are valid build sources; the DRM copy is
    /// unwrapped at build time).  Verified by bundle identifier and a 32-bit
    /// Intel executable.
    static func isSource(_ bundle: URL, for game: Game) -> Bool {
        guard bundleIdentifier(of: bundle) == game.id else { return false }
        let executable = bundle.appendingPathComponent(
            "Contents/MacOS/\(game.executableName)")
        return MachOInspector.isI386Executable(at: executable)
    }

    /// The game a dropped bundle belongs to, if any.
    static func game(of bundle: URL) -> Game? {
        Game.matching(bundleIdentifier: bundleIdentifier(of: bundle))
    }

    private static func bundleIdentifier(of bundle: URL) -> String? {
        let plistURL = bundle.appendingPathComponent("Contents/Info.plist")
        guard let data = try? Data(contentsOf: plistURL),
              let plist = try? PropertyListSerialization.propertyList(
                  from: data, options: [], format: nil) as? [String: Any] else {
            return nil
        }
        return plist["CFBundleIdentifier"] as? String
    }
}

private enum MachOInspector {
    private static let cpuTypeI386: UInt32 = 7
    private static let machHeader32: UInt32 = 0xfeedface
    private static let machHeader64: UInt32 = 0xfeedfacf
    private static let fatHeader: UInt32 = 0xcafebabe
    /// Valve's DRM wrapper appends its unlock stub, which carries the wrapper's
    /// own source paths; the native loader checks the same marker.
    private static let steamDRMMarker = Data("/src/drm/mach-o/".utf8)

    static func isI386Executable(at url: URL) -> Bool {
        guard let data = try? Data(contentsOf: url), data.count >= 12 else { return false }

        let magic = readUInt32(data, at: 0, bigEndian: false)
        switch magic {
        case machHeader32:
            return readUInt32(data, at: 4, bigEndian: false) == cpuTypeI386
        case machHeader64:
            return false
        case fatHeader:
            let architectureCount = Int(readUInt32(data, at: 4, bigEndian: true))
            guard architectureCount > 0,
                  architectureCount <= (data.count - 8) / 20 else { return false }
            for index in 0..<architectureCount {
                let architectureOffset = 8 + index * 20
                if readUInt32(data, at: architectureOffset, bigEndian: true) == cpuTypeI386 {
                    return true
                }
            }
            return false
        default:
            return false
        }
    }

    static func isSteamDRMWrapped(at url: URL) -> Bool {
        guard let data = try? Data(contentsOf: url) else { return false }
        return data.range(of: steamDRMMarker) != nil
    }

    private static func readUInt32(_ data: Data, at offset: Int, bigEndian: Bool) -> UInt32 {
        let bytes = data[offset..<(offset + 4)]
        let values = bytes.map(UInt32.init)
        if bigEndian {
            return (values[0] << 24) | (values[1] << 16) | (values[2] << 8) | values[3]
        }
        return values[0] | (values[1] << 8) | (values[2] << 16) | (values[3] << 24)
    }
}
