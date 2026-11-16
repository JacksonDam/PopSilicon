import AppKit
import Combine
import SwiftUI
import UniformTypeIdentifiers

let fileURLTypeIdentifier = "public.file-url"

struct SymbolImage: View {
    let name: String
    let size: CGFloat

    init(_ name: String, size: CGFloat) {
        self.name = name
        self.size = size
    }

    var body: some View {
        if #available(macOS 11.0, *) {
            return AnyView(Image(systemName: name))
        }
        return AnyView(
            LegacySymbol(name: name)
                .frame(width: size, height: size)
        )
    }
}

struct CompatLabel: View {
    private let title: Text
    private let systemImage: String
    private let iconSize: CGFloat

    init(_ titleKey: LocalizedStringKey, systemImage: String, iconSize: CGFloat = 14) {
        title = Text(titleKey)
        self.systemImage = systemImage
        self.iconSize = iconSize
    }

    @_disfavoredOverload
    init<S: StringProtocol>(_ title: S, systemImage: String, iconSize: CGFloat = 14) {
        self.title = Text(title)
        self.systemImage = systemImage
        self.iconSize = iconSize
    }

    var body: some View {
        if #available(macOS 11.0, *) {
            return AnyView(
                Label {
                    title
                } icon: {
                    Image(systemName: systemImage)
                }
            )
        }
        return AnyView(
            HStack(alignment: .center, spacing: 6) {
                LegacySymbol(name: systemImage)
                    .frame(width: iconSize, height: iconSize)
                title
            }
        )
    }
}

struct CompatSpinner: View {
    var body: some View {
        if #available(macOS 11.0, *) {
            return AnyView(
                ProgressView()
                    .controlSize(.small)
            )
        }
        return AnyView(
            LegacySpinner()
                .frame(width: 16, height: 16)
        )
    }
}

private struct LegacySpinner: NSViewRepresentable {
    func makeNSView(context: Context) -> NSProgressIndicator {
        let indicator = NSProgressIndicator()
        indicator.style = .spinning
        indicator.controlSize = .small
        indicator.isIndeterminate = true
        indicator.startAnimation(nil)
        return indicator
    }

    func updateNSView(_ indicator: NSProgressIndicator, context: Context) {}
}

struct CompatDisclosureGroup<Content: View>: View {
    let title: LocalizedStringKey
    @Binding var isExpanded: Bool
    @ViewBuilder let content: () -> Content

    init(
        _ title: LocalizedStringKey,
        isExpanded: Binding<Bool>,
        @ViewBuilder content: @escaping () -> Content
    ) {
        self.title = title
        _isExpanded = isExpanded
        self.content = content
    }

    var body: some View {
        if #available(macOS 11.0, *) {
            return AnyView(DisclosureGroup(title, isExpanded: $isExpanded, content: content))
        }
        return AnyView(legacyBody)
    }

    private var legacyBody: some View {
        VStack(alignment: .leading, spacing: 8) {
            Button {
                isExpanded.toggle()
            } label: {
                HStack(spacing: 6) {
                    LegacySymbol(name: isExpanded ? "chevron.down" : "chevron.right")
                        .frame(width: 9, height: 9)
                    Text(title)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(PlainButtonStyle())
            if isExpanded {
                content()
            }
        }
    }
}

extension Font {
    static var compatTitle2: Font {
        if #available(macOS 11.0, *) {
            return .title2
        }
        return .system(size: 17)
    }
}

extension View {
    func compatHelp(_ text: LocalizedStringKey) -> some View {
        if #available(macOS 11.0, *) {
            return AnyView(help(text))
        }
        return AnyView(self)
    }

    func compatAccessibilityLabel(_ label: LocalizedStringKey) -> some View {
        if #available(macOS 11.0, *) {
            return AnyView(accessibilityLabel(label))
        }
        return AnyView(accessibility(label: Text(label)))
    }

    func compatAccessibilityHint(_ hint: LocalizedStringKey) -> some View {
        if #available(macOS 11.0, *) {
            return AnyView(accessibilityHint(hint))
        }
        return AnyView(accessibility(hint: Text(hint)))
    }

    func compatLargeControlSize() -> some View {
        if #available(macOS 11.0, *) {
            return AnyView(controlSize(.large))
        }
        return AnyView(self)
    }

    func compatFileDrop(
        isTargeted: Binding<Bool>,
        perform: @escaping ([NSItemProvider]) -> Bool
    ) -> some View {
        if #available(macOS 11.0, *) {
            return AnyView(onDrop(of: [UTType.fileURL], isTargeted: isTargeted, perform: perform))
        }
        return AnyView(onDrop(of: [fileURLTypeIdentifier], isTargeted: isTargeted, perform: perform))
    }

    func compatOnChange<P: Publisher>(
        of value: P.Output,
        publisher: P,
        perform: @escaping (P.Output) -> Void
    ) -> some View where P.Output: Equatable, P.Failure == Never {
        if #available(macOS 11.0, *) {
            return AnyView(onChange(of: value, perform: perform))
        }
        return AnyView(onReceive(publisher.removeDuplicates().dropFirst(), perform: perform))
    }
}

struct LegacySymbol: View {
    let name: String

    var body: some View {
        GeometryReader { proxy in
            symbol(in: min(proxy.size.width, proxy.size.height))
                .frame(width: proxy.size.width, height: proxy.size.height)
        }
    }

    @ViewBuilder
    private func symbol(in side: CGFloat) -> some View {
        let line = max(1, side * 0.1)
        let round = StrokeStyle(lineWidth: line, lineCap: .round, lineJoin: .round)
        switch name {
        case "chevron.left":
            LegacyPath { r in
                [CGPoint(x: r.maxX - r.width * 0.32, y: r.minY + r.height * 0.12),
                 CGPoint(x: r.minX + r.width * 0.3, y: r.midY),
                 CGPoint(x: r.maxX - r.width * 0.32, y: r.maxY - r.height * 0.12)]
            }
            .stroke(style: StrokeStyle(lineWidth: max(1.5, side * 0.15), lineCap: .round, lineJoin: .round))
        case "chevron.right":
            LegacyPath { r in
                [CGPoint(x: r.minX + r.width * 0.32, y: r.minY + r.height * 0.12),
                 CGPoint(x: r.maxX - r.width * 0.3, y: r.midY),
                 CGPoint(x: r.minX + r.width * 0.32, y: r.maxY - r.height * 0.12)]
            }
            .stroke(style: StrokeStyle(lineWidth: max(1.5, side * 0.15), lineCap: .round, lineJoin: .round))
        case "chevron.down":
            LegacyPath { r in
                [CGPoint(x: r.minX + r.width * 0.12, y: r.minY + r.height * 0.32),
                 CGPoint(x: r.midX, y: r.maxY - r.height * 0.3),
                 CGPoint(x: r.maxX - r.width * 0.12, y: r.minY + r.height * 0.32)]
            }
            .stroke(style: StrokeStyle(lineWidth: max(1.5, side * 0.15), lineCap: .round, lineJoin: .round))
        case "arrow.down.app":
            ZStack {
                RoundedRectangle(cornerRadius: side * 0.24)
                    .inset(by: line / 2)
                    .stroke(lineWidth: line)
                LegacyDownArrow(lengthRatio: 0.5)
                    .stroke(style: round)
            }
        case "arrow.down.circle":
            ZStack {
                Circle().inset(by: line / 2).stroke(lineWidth: line)
                LegacyDownArrow(lengthRatio: 0.46)
                    .stroke(style: round)
            }
        case "exclamationmark.triangle", "exclamationmark.triangle.fill":
            LegacyWarning(filled: name.hasSuffix(".fill"), line: line)
        case "checkmark.circle", "checkmark.circle.fill":
            if name.hasSuffix(".fill") {
                ZStack {
                    Circle()
                    CheckmarkShape()
                        .stroke(style: StrokeStyle(lineWidth: line * 1.3, lineCap: .round, lineJoin: .round))
                        .padding(side * 0.24)
                        .blendMode(.destinationOut)
                }
                .compositingGroup()
            } else {
                ZStack {
                    Circle().inset(by: line / 2).stroke(lineWidth: line)
                    CheckmarkShape()
                        .stroke(style: round)
                        .padding(side * 0.24)
                }
            }
        case "gamecontroller":
            LegacyGameController(line: line)
        case "folder":
            LegacyFolder()
                .stroke(style: round)
        case "diamond.fill":
            LegacyPath(closed: true) { r in
                [CGPoint(x: r.midX, y: r.minY + r.height * 0.04),
                 CGPoint(x: r.maxX - r.width * 0.1, y: r.midY),
                 CGPoint(x: r.midX, y: r.maxY - r.height * 0.04),
                 CGPoint(x: r.minX + r.width * 0.1, y: r.midY)]
            }
            .fill()
        case "circle.hexagongrid.fill":
            LegacyDots(points: [(0, 0), (0, -1), (0, 1),
                                (-0.866, -0.5), (0.866, -0.5),
                                (-0.866, 0.5), (0.866, 0.5)],
                       spacing: 0.33, radius: 0.13)
        case "circle.grid.3x3.fill":
            LegacyDots(points: [-1.0, 0.0, 1.0].flatMap { y in
                [-1.0, 0.0, 1.0].map { x in (x, y) }
            }, spacing: 0.34, radius: 0.13)
        case "circle.circle.fill":
            ZStack {
                Circle()
                Circle()
                    .inset(by: side * 0.24)
                    .stroke(lineWidth: line * 1.2)
                    .blendMode(.destinationOut)
            }
            .compositingGroup()
        case "leaf.fill":
            LegacyLeaf().fill()
        case "book.fill":
            LegacyBook().fill()
        case "fish.fill":
            LegacyFish().fill()
        case "arrow.triangle.2.circlepath":
            LegacyCirclePath()
                .stroke(style: round)
        default:
            Circle().inset(by: line / 2).stroke(lineWidth: line)
        }
    }
}

private struct LegacyPath: Shape {
    var closed = false
    let points: @Sendable (CGRect) -> [CGPoint]

    init(closed: Bool = false, points: @escaping @Sendable (CGRect) -> [CGPoint]) {
        self.closed = closed
        self.points = points
    }

    func path(in rect: CGRect) -> Path {
        var path = Path()
        path.addLines(points(rect))
        if closed { path.closeSubpath() }
        return path
    }
}

private struct LegacyDownArrow: Shape {
    let lengthRatio: CGFloat

    func path(in rect: CGRect) -> Path {
        var path = Path()
        let top = rect.midY - rect.height * lengthRatio / 2
        let bottom = rect.midY + rect.height * lengthRatio / 2
        let wing = rect.width * lengthRatio * 0.42
        path.move(to: CGPoint(x: rect.midX, y: top))
        path.addLine(to: CGPoint(x: rect.midX, y: bottom))
        path.move(to: CGPoint(x: rect.midX - wing, y: bottom - wing))
        path.addLine(to: CGPoint(x: rect.midX, y: bottom))
        path.addLine(to: CGPoint(x: rect.midX + wing, y: bottom - wing))
        return path
    }
}

private struct LegacyWarning: View {
    let filled: Bool
    let line: CGFloat

    var body: some View {
        GeometryReader { proxy in
            let side = min(proxy.size.width, proxy.size.height)
            let triangle = LegacyPath(closed: true) { r in
                [CGPoint(x: r.midX, y: r.minY + r.height * 0.06),
                 CGPoint(x: r.maxX - r.width * 0.02, y: r.maxY - r.height * 0.06),
                 CGPoint(x: r.minX + r.width * 0.02, y: r.maxY - r.height * 0.06)]
            }
            let mark = ZStack {
                Capsule()
                    .frame(width: side * 0.11, height: side * 0.34)
                    .offset(y: side * 0.02)
                Circle()
                    .frame(width: side * 0.12, height: side * 0.12)
                    .offset(y: side * 0.28)
            }
            ZStack {
                if filled {
                    triangle.fill()
                    mark.blendMode(.destinationOut)
                } else {
                    triangle.stroke(style: StrokeStyle(lineWidth: line, lineJoin: .round))
                    mark
                }
            }
            .compositingGroup()
            .frame(width: proxy.size.width, height: proxy.size.height)
        }
    }
}

private struct LegacyGameController: View {
    let line: CGFloat

    var body: some View {
        GeometryReader { proxy in
            let side = min(proxy.size.width, proxy.size.height)
            ZStack {
                RoundedRectangle(cornerRadius: side * 0.26)
                    .stroke(lineWidth: line)
                    .frame(width: side * 0.94, height: side * 0.58)
                Rectangle()
                    .frame(width: side * 0.26, height: line * 0.9)
                    .offset(x: -side * 0.2)
                Rectangle()
                    .frame(width: line * 0.9, height: side * 0.26)
                    .offset(x: -side * 0.2)
                Circle()
                    .frame(width: side * 0.1, height: side * 0.1)
                    .offset(x: side * 0.18, y: -side * 0.06)
                Circle()
                    .frame(width: side * 0.1, height: side * 0.1)
                    .offset(x: side * 0.28, y: side * 0.06)
            }
            .frame(width: proxy.size.width, height: proxy.size.height)
        }
    }
}

private struct LegacyFolder: Shape {
    func path(in rect: CGRect) -> Path {
        let r = rect.insetBy(dx: rect.width * 0.06, dy: rect.height * 0.16)
        let tab = r.width * 0.38
        var path = Path()
        path.move(to: CGPoint(x: r.minX, y: r.maxY))
        path.addLine(to: CGPoint(x: r.minX, y: r.minY))
        path.addLine(to: CGPoint(x: r.minX + tab, y: r.minY))
        path.addLine(to: CGPoint(x: r.minX + tab + r.width * 0.1, y: r.minY + r.height * 0.16))
        path.addLine(to: CGPoint(x: r.maxX, y: r.minY + r.height * 0.16))
        path.addLine(to: CGPoint(x: r.maxX, y: r.maxY))
        path.closeSubpath()
        path.move(to: CGPoint(x: r.minX, y: r.minY + r.height * 0.32))
        path.addLine(to: CGPoint(x: r.maxX, y: r.minY + r.height * 0.32))
        return path
    }
}

private struct LegacyDots: View {
    let points: [(Double, Double)]
    let spacing: CGFloat
    let radius: CGFloat

    var body: some View {
        GeometryReader { proxy in
            let side = min(proxy.size.width, proxy.size.height)
            ZStack {
                ForEach(0..<points.count, id: \.self) { index in
                    Circle()
                        .frame(width: side * radius * 2, height: side * radius * 2)
                        .offset(x: CGFloat(points[index].0) * side * spacing,
                                y: CGFloat(points[index].1) * side * spacing)
                }
            }
            .frame(width: proxy.size.width, height: proxy.size.height)
        }
    }
}

private struct LegacyLeaf: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()
        let tip = CGPoint(x: rect.maxX - rect.width * 0.08, y: rect.minY + rect.height * 0.08)
        let base = CGPoint(x: rect.minX + rect.width * 0.14, y: rect.maxY - rect.height * 0.14)
        path.move(to: base)
        path.addQuadCurve(to: tip, control: CGPoint(x: rect.minX + rect.width * 0.02, y: rect.minY + rect.height * 0.1))
        path.addQuadCurve(to: base, control: CGPoint(x: rect.maxX - rect.width * 0.1, y: rect.maxY - rect.height * 0.02))
        path.closeSubpath()
        var stem = Path()
        stem.move(to: base)
        stem.addLine(to: CGPoint(x: rect.minX + rect.width * 0.04, y: rect.maxY - rect.height * 0.04))
        path.addPath(stem.strokedPath(StrokeStyle(lineWidth: rect.width * 0.08, lineCap: .round)))
        return path
    }
}

private struct LegacyBook: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()
        let top = rect.minY + rect.height * 0.18
        let bottom = rect.maxY - rect.height * 0.14
        let gap = rect.width * 0.04
        path.move(to: CGPoint(x: rect.midX - gap, y: top + rect.height * 0.04))
        path.addQuadCurve(to: CGPoint(x: rect.minX + rect.width * 0.04, y: top),
                          control: CGPoint(x: rect.minX + rect.width * 0.24, y: top - rect.height * 0.06))
        path.addLine(to: CGPoint(x: rect.minX + rect.width * 0.04, y: bottom))
        path.addQuadCurve(to: CGPoint(x: rect.midX - gap, y: bottom + rect.height * 0.04),
                          control: CGPoint(x: rect.minX + rect.width * 0.24, y: bottom - rect.height * 0.06))
        path.closeSubpath()
        path.move(to: CGPoint(x: rect.midX + gap, y: top + rect.height * 0.04))
        path.addQuadCurve(to: CGPoint(x: rect.maxX - rect.width * 0.04, y: top),
                          control: CGPoint(x: rect.maxX - rect.width * 0.24, y: top - rect.height * 0.06))
        path.addLine(to: CGPoint(x: rect.maxX - rect.width * 0.04, y: bottom))
        path.addQuadCurve(to: CGPoint(x: rect.midX + gap, y: bottom + rect.height * 0.04),
                          control: CGPoint(x: rect.maxX - rect.width * 0.24, y: bottom - rect.height * 0.06))
        path.closeSubpath()
        return path
    }
}

private struct LegacyFish: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()
        let body = CGRect(x: rect.minX + rect.width * 0.04, y: rect.minY + rect.height * 0.26,
                          width: rect.width * 0.68, height: rect.height * 0.48)
        path.addEllipse(in: body)
        path.move(to: CGPoint(x: body.maxX - rect.width * 0.06, y: rect.midY))
        path.addLine(to: CGPoint(x: rect.maxX - rect.width * 0.02, y: rect.minY + rect.height * 0.24))
        path.addLine(to: CGPoint(x: rect.maxX - rect.width * 0.02, y: rect.maxY - rect.height * 0.24))
        path.closeSubpath()
        return path
    }
}

private struct LegacyCirclePath: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()
        let center = CGPoint(x: rect.midX, y: rect.midY)
        let radius = min(rect.width, rect.height) * 0.36
        let head = radius * 0.42
        for start in [200.0, 20.0] {
            let end = start + 140
            path.addArc(center: center, radius: radius,
                        startAngle: .degrees(start), endAngle: .degrees(end), clockwise: false)
            let angle = end * .pi / 180
            let tip = CGPoint(x: center.x + radius * CGFloat(cos(angle)),
                              y: center.y + radius * CGFloat(sin(angle)))
            let back = angle - .pi / 2
            path.move(to: CGPoint(x: tip.x + head * CGFloat(cos(back - 0.5)),
                                  y: tip.y + head * CGFloat(sin(back - 0.5))))
            path.addLine(to: tip)
            path.addLine(to: CGPoint(x: tip.x + head * CGFloat(cos(back + 0.5)),
                                     y: tip.y + head * CGFloat(sin(back + 0.5))))
            path.move(to: CGPoint(x: center.x + radius * CGFloat(cos((start + 160) * .pi / 180)),
                                  y: center.y + radius * CGFloat(sin((start + 160) * .pi / 180))))
        }
        return path
    }
}
