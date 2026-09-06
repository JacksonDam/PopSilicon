import SwiftUI

struct CheckmarkShape: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()
        path.move(to: CGPoint(x: rect.minX + rect.width * 0.16, y: rect.midY))
        path.addLine(to: CGPoint(x: rect.minX + rect.width * 0.42, y: rect.maxY - rect.height * 0.18))
        path.addLine(to: CGPoint(x: rect.maxX - rect.width * 0.12, y: rect.minY + rect.height * 0.16))
        return path
    }
}
