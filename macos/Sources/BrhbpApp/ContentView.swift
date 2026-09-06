import BrhbpKit
import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @Bindable var model: PrintModel
    @State private var importing = false

    var body: some View {
        NavigationSplitView {
            SettingsSidebar(model: model, importing: $importing)
                .navigationSplitViewColumnWidth(min: 280, ideal: 300, max: 340)
        } detail: {
            PreviewPane(model: model)
        }
        .toolbar { toolbar }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.pdf]) { result in
            if case .success(let url) = result { model.load(url: url) }
        }
        .onReceive(NotificationCenter.default.publisher(for: .openDocument)) { _ in
            importing = true
        }
        .safeAreaInset(edge: .bottom) { StatusBar(model: model) }
    }

    @ToolbarContentBuilder
    private var toolbar: some ToolbarContent {
        ToolbarItem(placement: .navigation) {
            Button { importing = true } label: { Label("Open", systemImage: "doc.badge.plus") }
                .help("Choose a PDF")
        }
        ToolbarItemGroup(placement: .primaryAction) {
            if model.isPrinting {
                Button(role: .destructive) { model.cancel() } label: {
                    Label("Stop", systemImage: "stop.fill")
                }
                .buttonStyle(.glass)
                .disabled(model.cancelling)
                .help("Stop sending. Pages already sent will still print.")
            }
            Button { model.print() } label: { Label("Print", systemImage: "printer.fill") }
                .buttonStyle(.glassProminent)
                .disabled(!model.canPrint)
                .keyboardShortcut("p")
        }
    }
}

// MARK: - Sidebar

private struct SettingsSidebar: View {
    @Bindable var model: PrintModel
    @Binding var importing: Bool

    var body: some View {
        Form {
            Section("Printer") {
                LabeledContent("Address") {
                    TextField("address", text: $model.host)
                        .textFieldStyle(.roundedBorder)
                        .frame(maxWidth: 150)
                }
                DeviceBadge(device: model.device, hostIsEmpty: model.host.isEmpty)
            }

            Section("Paper") {
                Picker("Size", selection: $model.paper) {
                    ForEach(Paper.allCases) { Text($0.label).tag($0) }
                }
                Picker("Resolution", selection: $model.dpi) {
                    Text("300 dpi").tag(Int32(300))
                    Text("600 dpi").tag(Int32(600))
                    Text("1200 dpi").tag(Int32(1200))
                }
                Stepper("Copies: \(model.copies)", value: $model.copies, in: 1...999)
                Picker("Two-sided", selection: $model.duplex) {
                    ForEach(DuplexMode.allCases) { Text($0.label).tag($0) }
                }
                if model.duplex != .off {
                    Text(model.duplex.detail)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Toggle("Toner save", isOn: $model.tonerSave)
            }

            Section {
                Picker("Halftone", selection: $model.halftone) {
                    ForEach(Halftone.allCases) { Text($0.label).tag($0) }
                }
                Text(model.halftone.detail)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text("Rendering")
            }

            if model.device.reachable && !model.device.faults.isEmpty {
                Section("Attention") {
                    ForEach(model.device.faults, id: \.self) { f in
                        Label(f, systemImage: "exclamationmark.triangle.fill")
                            .foregroundStyle(.orange)
                    }
                }
            }
        }
        .formStyle(.grouped)
    }
}

private struct DeviceBadge: View {
    let device: DeviceSnapshot
    var hostIsEmpty = false

    private var tint: Color {
        guard device.reachable else { return .secondary }
        if !device.faults.isEmpty { return .orange }
        return device.state == .printing ? .blue : .green
    }

    var body: some View {
        HStack(spacing: 8) {
            Circle().fill(tint).frame(width: 8, height: 8)
            Text(device.reachable ? device.state.label
                                  : (hostIsEmpty ? "No address set" : "Not reachable"))
                .foregroundStyle(device.reachable ? .primary : .secondary)
            Spacer()
            if device.impressions >= 0 {
                Text("\(device.impressions)")
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.tertiary)
                    .help("Lifetime impressions")
            }
        }
    }
}

// MARK: - Preview

private struct PreviewPane: View {
    @Bindable var model: PrintModel

    var body: some View {
        if model.documentURL == nil {
            ContentUnavailableView {
                Label("No document", systemImage: "doc.text")
            } description: {
                Text("Choose a PDF to print.")
            } actions: {
                Button("Choose PDF…") {
                    NotificationCenter.default.post(name: .openDocument, object: nil)
                }
                .buttonStyle(.borderedProminent)
            }
        } else {
            ScrollView {
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 190), spacing: 20)], spacing: 20) {
                    ForEach(0..<model.pageCount, id: \.self) { i in
                        // The model is passed down rather than the image.
                        // ForEach's content closure runs outside this view's
                        // observation scope, so reading model.previews[i] here
                        // would never invalidate anything: the thumbnails were
                        // being rendered and silently never shown. Reading it
                        // inside PageThumb.body puts it back under tracking.
                        // .task rather than .onAppear: when the page count
                        // goes 0 -> N in the same pass the grid appears,
                        // onAppear is not reliably delivered for the items
                        // that are already on screen, so the first screenful
                        // sat on spinners until a scroll nudged it.
                        PageThumb(model: model, index: i)
                            .task(id: i) { model.renderPreview(page: i) }
                    }
                }
                .padding(24)
            }
            .background(.background.secondary)
            .contentMargins(.bottom, 64, for: .scrollContent)
        }
    }
}

private struct PageThumb: View {
    let model: PrintModel
    let index: Int

    var body: some View {
        let image = model.previews[index]
        let isCurrent = model.isPrinting && model.currentPage == index + 1
        return content(image: image, isCurrent: isCurrent)
    }

    private func content(image: CGImage?, isCurrent: Bool) -> some View {
        VStack(spacing: 6) {
            ZStack {
                RoundedRectangle(cornerRadius: 6).fill(.white)
                if let image {
                    Image(decorative: image, scale: 1)
                        .resizable().scaledToFit()
                } else {
                    ProgressView().controlSize(.small)
                }
            }
            .aspectRatio(1 / 1.414, contentMode: .fit)
            .clipShape(RoundedRectangle(cornerRadius: 6))
            .overlay {
                RoundedRectangle(cornerRadius: 6)
                    .stroke(isCurrent ? Color.accentColor : .black.opacity(0.12),
                            lineWidth: isCurrent ? 2.5 : 0.5)
            }
            .shadow(color: .black.opacity(0.15), radius: 5, y: 2)

            Text("\(index + 1)")
                .font(.caption.monospacedDigit())
                .foregroundStyle(isCurrent ? Color.accentColor : .secondary)
        }
    }
}

// MARK: - Status bar

/// Floats over the page grid rather than sitting in a chrome strip, so it is
/// one of the few places Liquid Glass genuinely applies: content moves behind
/// it. Glassing everything is the anti-pattern; this is the exception.
private struct StatusBar: View {
    @Bindable var model: PrintModel
    @Namespace private var glass

    var body: some View {
        GlassEffectContainer(spacing: 14) {
            HStack(spacing: 10) {
                HStack(spacing: 9) {
                    if model.isPrinting {
                        ProgressView().controlSize(.small)
                    } else {
                        Image(systemName: model.canPrint ? "printer" : "doc.text")
                            .foregroundStyle(.secondary)
                    }
                    Text(model.statusLine)
                        .font(.callout)
                        .lineLimit(1)
                        .contentTransition(.numericText())
                }
                .padding(.horizontal, 15)
                .padding(.vertical, 9)
                .glassEffect(.regular, in: .capsule)
                .glassEffectID("status", in: glass)

                if model.isPrinting || model.cancelling {
                    Button("Cancel on Printer") { model.cancelOnDevice() }
                        .font(.callout)
                        .padding(.horizontal, 13)
                        .padding(.vertical, 9)
                        .glassEffect(.regular.interactive(), in: .capsule)
                        .glassEffectID("device-cancel", in: glass)
                        .buttonStyle(.plain)
                        .help("Ask the device to drop pages it has already accepted")
                }
            }
            .animation(.smooth(duration: 0.28), value: model.isPrinting)
            .animation(.smooth(duration: 0.28), value: model.statusLine)
        }
        .padding(.bottom, 16)
    }
}
