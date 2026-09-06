import Foundation

enum SteamLocator {
    enum InstallationState: Equatable {
        /// Valve's copy of the game is still in place.
        case unpatched
        /// PeggleSilicon is installed and its game image is loadable.
        case peggleSilicon
        /// PeggleSilicon is installed, but its game image is Steam's still
        /// DRM-encrypted executable, which the loader cannot start (an older
        /// install predating in-installer DRM unwrapping); it can be repaired.
        case needsRepair
        case unsupported
    }

    static func find() -> URL? {
        let home = FileManager.default.homeDirectoryForCurrentUser
        let installation = home
            .appendingPathComponent("Library/Application Support/Steam", isDirectory: true)
            .appendingPathComponent("steamapps/common/Peggle Deluxe/Peggle Deluxe.app", isDirectory: true)
        let executable = installation.appendingPathComponent("Contents/MacOS/Peggle")

        guard FileManager.default.fileExists(atPath: installation.path),
              FileManager.default.fileExists(atPath: executable.path) else {
            return nil
        }
        return installation
    }

    static func state(of installation: URL) -> InstallationState {
        let contents = installation.appendingPathComponent("Contents")
        let executable = contents.appendingPathComponent("MacOS/Peggle")
        let resources = contents.appendingPathComponent("Resources/main.pak")
        let sharedImage = contents.appendingPathComponent("SharedSupport/Peggle.image")

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

    /// Whether a bundle's executable is the original 32-bit Peggle game (retail
    /// or Steam's DRM copy — both are valid build sources; the DRM copy is
    /// unwrapped at build time).
    static func isGameSource(_ bundle: URL) -> Bool {
        MachOInspector.isI386Executable(at: bundle.appendingPathComponent("Contents/MacOS/Peggle"))
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
