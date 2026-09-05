import Foundation

enum SteamReplacementRunner {
    static func run(source: URL, target: URL, projectRoot: URL) async -> BuildResult {
        let fileManager = FileManager.default
        let parent = target.deletingLastPathComponent()
        let backup = target.appendingPathExtension("bak")
        let temporaryOutput = parent.appendingPathComponent(".PeggleSilicon.install.app")
        let originalInfo = readInfoPlist(at: target)

        guard !fileManager.fileExists(atPath: backup.path) else {
            return BuildResult(
                exitStatus: -1,
                output: "A backup already exists at \(backup.path). Remove or move it before replacing the Steam installation."
            )
        }
        guard !fileManager.fileExists(atPath: temporaryOutput.path) else {
            return BuildResult(
                exitStatus: -1,
                output: "A previous temporary installation exists at \(temporaryOutput.path). Remove it before trying again."
            )
        }

        let build = await BuildRunner.run(
            source: source,
            destination: temporaryOutput,
            projectRoot: projectRoot
        )
        guard build.succeeded else { return build }

        do {
            try makeSteamCompatible(at: temporaryOutput, preserving: originalInfo)
            try fileManager.moveItem(at: target, to: backup)
            do {
                try fileManager.moveItem(at: temporaryOutput, to: target)
            } catch {
                try? fileManager.moveItem(at: backup, to: target)
                throw error
            }
        } catch {
            try? fileManager.removeItem(at: temporaryOutput)
            return BuildResult(
                exitStatus: -1,
                output: build.output + "\nUnable to replace the Steam installation: \(error.localizedDescription)"
            )
        }

        return BuildResult(
            exitStatus: 0,
            output: build.output + "\nSteam installation replaced. Original saved to \(backup.path)."
        )
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
        preserving originalInfo: [String: Any]?
    ) throws {
        let fileManager = FileManager.default
        let macOSDirectory = bundle.appendingPathComponent("Contents/MacOS")
        let siliconExecutable = macOSDirectory.appendingPathComponent("PeggleSilicon")
        let steamExecutable = macOSDirectory.appendingPathComponent("Peggle")

        guard fileManager.fileExists(atPath: siliconExecutable.path) else {
            throw InstallerError("The generated PeggleSilicon executable was not found.")
        }
        guard !fileManager.fileExists(atPath: steamExecutable.path) else {
            throw InstallerError("The temporary Steam installation already contains a Peggle executable.")
        }

        // Steam's macOS launch configuration invokes the original executable
        // name directly, so the compatibility loader must be exposed as Peggle.
        try fileManager.moveItem(at: siliconExecutable, to: steamExecutable)

        let plistURL = bundle.appendingPathComponent("Contents/Info.plist")
        guard var plist = readInfoPlist(at: bundle) else {
            throw InstallerError("The generated app has an invalid Info.plist.")
        }
        plist["CFBundleExecutable"] = "Peggle"
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
