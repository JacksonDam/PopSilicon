import Foundation

enum SteamReplacementRunner {
    /// Builds PeggleSilicon from `source` (the bundle whose executable becomes
    /// the game image) and installs it at `target`, Steam's app location.
    /// A target that is still Valve's copy is renamed to `.bak` first; a
    /// target that already holds PeggleSilicon is replaced in place, keeping
    /// whatever backup exists.
    static func run(source: URL, target: URL, game: Game, projectRoot: URL) async -> BuildResult {
        let fileManager = FileManager.default
        let parent = target.deletingLastPathComponent()
        let backup = target.appendingPathExtension("bak")
        let temporaryOutput = parent.appendingPathComponent(".PeggleSilicon.install.app")
        let previousInstall = parent.appendingPathComponent(".PeggleSilicon.previous.app")
        let backupExists = fileManager.fileExists(atPath: backup.path)
        let targetIsPeggleSilicon = fileManager.fileExists(
            atPath: target.appendingPathComponent("Contents/SharedSupport/\(game.imageFileName)").path
        )
        let originalInfo = readInfoPlist(at: backupExists ? backup : target)

        guard targetIsPeggleSilicon || !backupExists else {
            return BuildResult(
                exitStatus: -1,
                output: "A backup already exists at \(backup.path). Remove or move it before replacing the Steam installation."
            )
        }
        for leftover in [temporaryOutput, previousInstall] where fileManager.fileExists(atPath: leftover.path) {
            return BuildResult(
                exitStatus: -1,
                output: "A previous temporary installation exists at \(leftover.path). Remove it before trying again."
            )
        }

        let build = await BuildRunner.run(
            source: source,
            destination: temporaryOutput,
            projectRoot: projectRoot
        )
        guard build.succeeded else { return build }

        do {
            try makeSteamCompatible(at: temporaryOutput, game: game, preserving: originalInfo)
            // Move the current bundle aside, then put the new one in place;
            // the aside copy is restored if that fails.
            let aside = targetIsPeggleSilicon ? previousInstall : backup
            try fileManager.moveItem(at: target, to: aside)
            do {
                try fileManager.moveItem(at: temporaryOutput, to: target)
            } catch {
                try? fileManager.moveItem(at: aside, to: target)
                throw error
            }
            if targetIsPeggleSilicon {
                try? fileManager.removeItem(at: previousInstall)
            }
        } catch {
            try? fileManager.removeItem(at: temporaryOutput)
            return BuildResult(
                exitStatus: -1,
                output: build.output + "\nUnable to replace the Steam installation: \(error.localizedDescription)"
            )
        }

        let summary: String
        if !targetIsPeggleSilicon {
            summary = "Steam installation replaced. Original saved to \(backup.path)."
        } else if backupExists {
            summary = "Steam installation repaired. The original remains at \(backup.path)."
        } else {
            summary = "Steam installation repaired."
        }
        return BuildResult(exitStatus: 0, output: build.output + "\n" + summary)
    }

    private static func readInfoPlist(at bundle: URL) -> [String: Any]? {
        let plistURL = bundle.appendingPathComponent("Contents/Info.plist")
        guard let data = try? Data(contentsOf: plistURL),
              let plist = try? PropertyListSerialization.propertyList(
                  from: data,
                  options: [],
                  format: nil
              ) as? [String: Any] else {
            return nil
        }
        return plist
    }

    private static func makeSteamCompatible(
        at bundle: URL,
        game: Game,
        preserving originalInfo: [String: Any]?
    ) throws {
        let fileManager = FileManager.default
        let macOSDirectory = bundle.appendingPathComponent("Contents/MacOS")
        let siliconExecutable = macOSDirectory.appendingPathComponent("PeggleSilicon")
        let steamExecutable = macOSDirectory.appendingPathComponent(game.executableName)

        guard fileManager.fileExists(atPath: siliconExecutable.path) else {
            throw InstallerError("The generated PeggleSilicon executable was not found.")
        }
        guard !fileManager.fileExists(atPath: steamExecutable.path) else {
            throw InstallerError("The temporary Steam installation already contains a \(game.executableName) executable.")
        }

        // Steam's macOS launch configuration invokes the original executable
        // name directly, so the compatibility loader must be exposed under it.
        try fileManager.moveItem(at: siliconExecutable, to: steamExecutable)

        let plistURL = bundle.appendingPathComponent("Contents/Info.plist")
        guard var plist = readInfoPlist(at: bundle) else {
            throw InstallerError("The generated app has an invalid Info.plist.")
        }
        plist["CFBundleExecutable"] = game.executableName
        for key in ["CFBundleIdentifier", "CFBundleName", "CFBundleDisplayName"] {
            if let value = originalInfo?[key] {
                plist[key] = value
            }
        }
        let plistData = try PropertyListSerialization.data(
            fromPropertyList: plist,
            format: .xml,
            options: 0
        )
        try plistData.write(to: plistURL, options: .atomic)

        try sign(bundle)
    }

    private static func sign(_ bundle: URL) throws {
        let process = Process()
        let pipe = Pipe()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/codesign")
        process.arguments = ["--force", "--deep", "--sign", "-", bundle.path]
        process.standardOutput = pipe
        process.standardError = pipe
        try process.run()
        process.waitUntilExit()

        guard process.terminationStatus == 0 else {
            let output = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
            throw InstallerError(
                output.isEmpty
                    ? "The Steam-compatible app could not be signed."
                    : "The Steam-compatible app could not be signed: \(output.trimmingCharacters(in: .whitespacesAndNewlines))"
            )
        }
    }
}

private struct InstallerError: LocalizedError {
    let message: String

    init(_ message: String) {
        self.message = message
    }

    var errorDescription: String? { message }
}
