import Foundation

struct BuildResult: Sendable {
    let exitStatus: Int32
    let output: String

    var succeeded: Bool { exitStatus == 0 }
}

enum BuildRunner {
    static func run(source: URL, destination: URL, projectRoot: URL) async -> BuildResult {
        await withCheckedContinuation { continuation in
            let process = Process()
            let pipe = Pipe()
            let buildScript = projectRoot.appendingPathComponent("tools/run_build.sh")

            process.executableURL = URL(fileURLWithPath: "/bin/sh")
            process.arguments = [
                buildScript.path,
                source.path,
                "--output",
                destination.path,
            ]
            process.currentDirectoryURL = projectRoot
            process.standardOutput = pipe
            process.standardError = pipe
            process.terminationHandler = { process in
                let data = pipe.fileHandleForReading.readDataToEndOfFile()
                let output = String(decoding: data, as: UTF8.self)
                continuation.resume(
                    returning: BuildResult(
                        exitStatus: process.terminationStatus,
                        output: output
                    )
                )
            }

            do {
                try process.run()
            } catch {
                continuation.resume(
                    returning: BuildResult(exitStatus: -1, output: error.localizedDescription)
                )
            }
        }
    }
}
