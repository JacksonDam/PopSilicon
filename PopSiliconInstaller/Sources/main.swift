import AppKit

if #available(macOS 11.0, *) {
    PopSiliconInstallerApp.main()
} else {
    LegacyInstallerApp.run()
}
