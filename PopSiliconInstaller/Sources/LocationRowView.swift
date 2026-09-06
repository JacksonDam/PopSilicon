import SwiftUI

struct LocationRowView: View {
    let title: String
    let value: String
    let action: () -> Void

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Label(title, systemImage: "folder")
                .frame(width: 140, alignment: .leading)
            Text(value)
                .lineLimit(2)
                .truncationMode(.middle)
                .foregroundColor(value == "Choose a folder" ? .gray : .primary)
            Spacer(minLength: 0)
            Button("Choose…", action: action)
        }
    }
}
