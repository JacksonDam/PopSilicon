import Darwin
import Foundation

@MainActor
final class PegHitSoundPlayer {
    private var player: BassSoundPlayer?
    private var playbackID = UUID()

    func play(from gameBundle: URL, projectRoot: URL) {
        stop()

        guard let soundData = PegHitSoundExtractor.extract(from: gameBundle),
              let player = BassSoundPlayer(
                  libraryURL: projectRoot.appendingPathComponent("native/vendor/bass/libbass.dylib")
              ),
              player.play(soundData) else {
            return
        }

        self.player = player
        let playbackID = UUID()
        self.playbackID = playbackID
        Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 2_000_000_000)
            guard let self, self.playbackID == playbackID else { return }
            self.stop()
        }
    }

    func stop() {
        playbackID = UUID()
        player?.stop()
        player = nil
    }
}

private final class BassSoundPlayer {
    private typealias BassInit = @convention(c) (
        Int32, UInt32, UInt32, UnsafeMutableRawPointer?, UnsafeRawPointer?
    ) -> Int32
    private typealias BassFree = @convention(c) () -> Int32
    private typealias BassStreamCreateFile = @convention(c) (
        UInt32, UnsafeRawPointer?, UInt64, UInt64, UInt32
    ) -> UInt32
    private typealias BassStreamFree = @convention(c) (UInt32) -> Int32
    private typealias BassChannelPlay = @convention(c) (UInt32, Int32) -> Int32

    private static let fileMemoryCopy: UInt32 = 3
    private let library: UnsafeMutableRawPointer
    private let copyDirectory: URL?
    private let bassFree: BassFree
    private let streamCreateFile: BassStreamCreateFile
    private let streamFree: BassStreamFree
    private let channelPlay: BassChannelPlay
    private var initialized = true
    private var stream: UInt32 = 0
    private var soundData: Data?

    init?(libraryURL: URL) {
        // A checkout downloaded as an archive carries a quarantine attribute
        // on the vendor dylib, and Gatekeeper prompts before it lets a
        // quarantined library into a Finder-launched app.  Load a private copy
        // taken without extended attributes instead; the arm64 slice is
        // already ad hoc signed, so nothing else is needed.
        let copy = Self.copyWithoutAttributes(of: libraryURL)
        let loadURL = copy?.file ?? libraryURL
        guard let library = dlopen(loadURL.path, RTLD_NOW | RTLD_LOCAL) else {
            Self.removeCopy(copy)
            return nil
        }
        self.library = library
        self.copyDirectory = copy?.directory

        func load<T>(_ name: String, as type: T.Type) -> T? {
            guard let symbol = dlsym(library, name) else { return nil }
            return unsafeBitCast(symbol, to: type)
        }

        guard let bassInit = load("BASS_Init", as: BassInit.self),
              let bassFree = load("BASS_Free", as: BassFree.self),
              let streamCreateFile = load("BASS_StreamCreateFile", as: BassStreamCreateFile.self),
              let streamFree = load("BASS_StreamFree", as: BassStreamFree.self),
              let channelPlay = load("BASS_ChannelPlay", as: BassChannelPlay.self),
              bassInit(-1, 44100, 0, nil, nil) != 0 else {
            dlclose(library)
            Self.removeCopy(copy)
            return nil
        }

        self.bassFree = bassFree
        self.streamCreateFile = streamCreateFile
        self.streamFree = streamFree
        self.channelPlay = channelPlay
    }

    func play(_ data: Data) -> Bool {
        soundData = data
        guard let soundData else {
            return false
        }
        let stream = soundData.withUnsafeBytes { bytes in
            streamCreateFile(
                Self.fileMemoryCopy,
                bytes.baseAddress,
                0,
                UInt64(bytes.count),
                0
            )
        }
        guard stream != 0 else { return false }
        self.stream = stream
        return channelPlay(stream, 1) != 0
    }

    func stop() {
        if stream != 0 {
            _ = streamFree(stream)
            stream = 0
        }
        soundData = nil
        if initialized {
            _ = bassFree()
            initialized = false
        }
    }

    deinit {
        if stream != 0 { _ = streamFree(stream) }
        if initialized { _ = bassFree() }
        dlclose(library)
        if let copyDirectory { try? FileManager.default.removeItem(at: copyDirectory) }
    }

    private static func copyWithoutAttributes(of source: URL) -> (directory: URL, file: URL)? {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("PopSiliconInstaller-\(getpid())", isDirectory: true)
        let file = directory.appendingPathComponent(source.lastPathComponent)
        guard (try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)) != nil else {
            return nil
        }
        // copyfile propagates the quarantine attribute even when asked for the
        // data alone, so strip it from the copy explicitly.
        guard copyfile(source.path, file.path, nil, copyfile_flags_t(COPYFILE_DATA)) == 0,
              removexattr(file.path, "com.apple.quarantine", 0) == 0 || errno == ENOATTR else {
            try? FileManager.default.removeItem(at: directory)
            return nil
        }
        return (directory, file)
    }

    private static func removeCopy(_ copy: (directory: URL, file: URL)?) {
        guard let copy else { return }
        try? FileManager.default.removeItem(at: copy.directory)
    }
}

private enum PegHitSoundExtractor {
    private static let soundPath = "sounds/peghit.ogg"
    private static let xorKey: UInt8 = 0xf7

    static func extract(from gameBundle: URL) -> Data? {
        let pakURL = gameBundle.appendingPathComponent("Contents/Resources/main.pak")
        guard let packedData = try? Data(contentsOf: pakURL) else { return nil }

        let data = packedData.map { $0 ^ xorKey }
        guard data.count >= 8,
              data[0] == 0xc0, data[1] == 0x4a,
              data[2] == 0xc0, data[3] == 0xba else { return nil }

        var offset = 8
        var records: [(name: String, size: Int)] = []
        while offset < data.count {
            let flag = data[offset]
            offset += 1
            if flag == 0x80 { break }
            guard offset < data.count else { return nil }
            let nameLength = Int(data[offset])
            offset += 1
            guard nameLength <= data.count - offset,
                  nameLength <= data.count - offset - 12 else { return nil }

            let nameData = Data(data[offset..<(offset + nameLength)])
            let name = String(decoding: nameData, as: UTF8.self)
                .replacingOccurrences(of: "\\", with: "/")
            offset += nameLength
            guard let size = readUInt32(data, at: offset) else { return nil }
            offset += 4
            guard data.count - offset >= 8 else { return nil }
            offset += 8
            records.append((name, size))
        }

        var dataOffset = offset
        for record in records {
            guard record.size <= data.count - dataOffset else { return nil }
            if record.name.caseInsensitiveCompare(soundPath) == .orderedSame {
                return Data(data[dataOffset..<(dataOffset + record.size)])
            }
            dataOffset += record.size
        }
        return nil
    }

    private static func readUInt32(_ data: [UInt8], at offset: Int) -> Int? {
        guard offset >= 0, data.count - offset >= 4 else { return nil }
        return Int(data[offset])
            | (Int(data[offset + 1]) << 8)
            | (Int(data[offset + 2]) << 16)
            | (Int(data[offset + 3]) << 24)
    }
}
