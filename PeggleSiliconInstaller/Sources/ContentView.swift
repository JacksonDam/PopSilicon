import AppKit
import SwiftUI

struct ContentView: View {
    @ObservedObject var model: InstallerModel

    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            VStack(alignment: .leading, spacing: 6) {
                Text("PeggleSilicon Installer")
                    .font(.largeTitle.bold())
                Text("Run Peggle Deluxe, Peggle Nights or Bejeweled 3 on Apple silicon")
                    .foregroundColor(.gray)
            }

            HStack(spacing: 10) {
                Text("Game")
                    .foregroundColor(.gray)
                Picker("Game", selection: $model.selectedGame) {
                    ForEach(model.availableGames) { game in
                        Text(game.displayName).tag(game)
                    }
                }
                .labelsHidden()
                .frame(maxWidth: 220)
                .disabled(model.isBuilding)
                Spacer()
            }

            if model.steamInstallationURL != nil {
                SteamInstallationView(model: model)
            }

            DropZoneView(model: model)

            Group {
                LocationRowView(
                    title: "Export location",
                    value: model.destinationURL?.path ?? "Choose a folder"
                ) {
                    model.chooseDestination()
                }

                if let destinationURL = model.destinationURL {
                    Text("The app will be saved as \(destinationURL.lastPathComponent).")
                        .font(.caption)
                        .foregroundColor(.gray)
                }
            }

            HStack {
                if model.isBuilding {
                    ProgressView()
                        .controlSize(.small)
                    Text("Building PeggleSilicon…")
                        .foregroundColor(.gray)
                } else {
                    Text(model.statusMessage)
                        .foregroundColor(.gray)
                }

                Spacer()

                Button("Install PeggleSilicon", action: model.export)
                .buttonStyle(DefaultButtonStyle())
                .controlSize(.large)
                .disabled(!model.canExport)
            }

            if let errorMessage = model.errorMessage {
                Label(errorMessage, systemImage: "exclamationmark.triangle.fill")
                    .foregroundColor(.red)
                    .fixedSize(horizontal: false, vertical: true)
            }

            if let buildOutput = model.buildOutput, !buildOutput.isEmpty {
                DisclosureGroup("Build output", isExpanded: $model.isBuildOutputExpanded) {
                    ScrollView([.vertical, .horizontal]) {
                        Text(buildOutput)
                            .font(.system(.caption, design: .monospaced))
                            .frame(maxWidth: .infinity, alignment: .topLeading)
                    }
                    .frame(minHeight: 180, idealHeight: 280, maxHeight: 360)
                    .padding(8)
                    .background(Color.white.opacity(0.06))
                    .clipShape(RoundedRectangle(cornerRadius: 6))
                    .overlay(
                        RoundedRectangle(cornerRadius: 6)
                            .stroke(Color.gray.opacity(0.3))
                    )
                }
                .onChange(of: model.isBuildOutputExpanded) { expanded in
                    guard expanded else { return }
                    DispatchQueue.main.async {
                        expandWindowForBuildOutput()
                    }
                }
            }
        }
        .padding(28)
        .frame(minWidth: 520, idealWidth: 560, minHeight: 520, idealHeight: 600)
        .alert(isPresented: $model.showSteamReplacementConfirmation) {
            Alert(
                title: Text(model.steamAlertTitle),
                message: Text(model.steamAlertMessage),
                primaryButton: .destructive(
                    Text(model.steamAlertButtonTitle),
                    action: model.confirmSteamReplacement
                ),
                secondaryButton: .cancel(Text("Cancel"))
            )
        }
    }

    private func expandWindowForBuildOutput() {
        guard let window = NSApp.keyWindow ?? NSApp.windows.first(where: {
            $0.title == "Install PeggleSilicon"
        }) else { return }

        let minimumHeight: CGFloat = 820
        guard window.frame.height < minimumHeight else { return }

        var frame = window.frame
        frame.origin.y -= minimumHeight - frame.height
        frame.size.height = minimumHeight
        window.setFrame(frame, display: true, animate: true)
    }

}
