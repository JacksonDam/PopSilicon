import Foundation

struct BuildResult: Sendable {
    let exitStatus: Int32
    let output: String

    var succeeded: Bool { exitStatus == 0 }
}

private final class OutputCollector: @unchecked Sendable {
    private let lock = NSLock()
    private var output = Data()
    private var pendingLine = Data()
    private let onLine: (@Sendable (String) -> Void)?

    init(onLine: (@Sendable (String) -> Void)?) {
        self.onLine = onLine
    }

    func append(_ chunk: Data) {
        guard !chunk.isEmpty else { return }
        var lines: [String] = []
        lock.lock()
        output.append(chunk)
        if onLine != nil {
            pendingLine.append(chunk)
            while let newline = pendingLine.firstIndex(of: UInt8(ascii: "\n")) {
                lines.append(String(decoding: pendingLine[pendingLine.startIndex..<newline], as: UTF8.self))
                pendingLine.removeSubrange(pendingLine.startIndex...newline)
            }
        }
        lock.unlock()
        for line in lines {
            onLine?(line)
        }
    }

    func finish() -> String {
        lock.lock()
        let remainder = pendingLine
        pendingLine.removeAll()
        let text = String(decoding: output, as: UTF8.self)
        lock.unlock()
        if !remainder.isEmpty {
            onLine?(String(decoding: remainder, as: UTF8.self))
        }
        return text
    }
}

private final class RunningBuild: @unchecked Sendable {
    let process = Process()
    let pipe = Pipe()
}

enum BuildRunner {
    static func run(
        source: URL,
        destination: URL,
        projectRoot: URL,
        onOutputLine: (@Sendable (String) -> Void)? = nil
    ) async -> BuildResult {
        await withCheckedContinuation { continuation in
            let build = RunningBuild()
            let collector = OutputCollector(onLine: onOutputLine)
            let buildScript = projectRoot.appendingPathComponent("tools/run_build.sh")

            build.process.executableURL = URL(fileURLWithPath: "/bin/sh")
            build.process.arguments = [
                buildScript.path,
                source.path,
                "--output",
                destination.path,
            ]
            build.process.currentDirectoryURL = projectRoot
            build.process.standardOutput = build.pipe
            build.process.standardError = build.pipe

            do {
                try build.process.run()
            } catch {
                continuation.resume(
                    returning: BuildResult(exitStatus: -1, output: error.localizedDescription)
                )
                return
            }

            let reader = Thread {
                let handle = build.pipe.fileHandleForReading
                while true {
                    let chunk = handle.availableData
                    if chunk.isEmpty { break }
                    collector.append(chunk)
                }
                build.process.waitUntilExit()
                continuation.resume(
                    returning: BuildResult(
                        exitStatus: build.process.terminationStatus,
                        output: collector.finish()
                    )
                )
            }
            reader.name = "PopSilicon build output"
            reader.start()
        }
    }
}
