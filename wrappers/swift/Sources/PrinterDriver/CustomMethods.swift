import CPrinterDriver
import Foundation

// Custom method registration (docs/api.md §16). The marquee is the custom completion
// mechanism: a vendor idle/ack scheme becomes a real graded completion path with no core
// release. The engine sends the registered fence behind the payload and routes the
// printer's continuous response stream through the matcher; a `.matched(token)` confirms
// the job exactly like `GS ( H`, with the same per-job token map and the same
// journalled, resolvable verification identifier.
//
// The callbacks are bridged to the C ABI the same way ``BluetoothTransport`` is: a
// trampoline holds the Swift closures alive (retained by the driver, because pd.h has no
// unsubscribe), non-capturing `@convention(c)` thunks recover it from the context pointer,
// and everything the SDK does about honesty stays on the core's side of the boundary.
//
// - Important: the callbacks run on the core's own threads — the fence on the printer's
//   worker thread, the matcher on the transport reader path. They must not block and must
//   not call back into the driver.

// MARK: - Completion method

/// A matcher's verdict on the printer→host bytes it was handed.
public enum CompletionMatch: Sendable {
  /// A fence answer carrying this four-character correlation token; confirms like `GS ( H`.
  case matched(String)
  /// Not this mechanism's bytes; the core drops its matcher buffer and reads on.
  case notMine
  /// An answer may be forming but is incomplete; the core keeps buffering and asks again.
  case needMore

  var cKind: pd_match_kind {
    switch self {
    case .matched: return PD_MATCH_MATCHED
    case .notMine: return PD_MATCH_NOT_MINE
    case .needMore: return PD_MATCH_NEED_MORE
    }
  }

  /// The core's own spelling of this verdict, from `pd_match_kind_name`.
  public var abiName: String { String(cString: pd_match_kind_name(cKind)) }
}

/// A registered custom completion mechanism (docs/api.md §16).
///
/// Bind a printer to it by attaching with the profile id `"vendoridle:<id>"`, e.g.
/// `"vendoridle:acme.x-idle"`, which resolves to a generic ESC/POS profile whose
/// completion is this method. A job on such a printer reports the ``grade``,
/// ``authority`` and ``methodName`` declared here, attributed by id in the result and in
/// `pdctl verify`.
public struct CompletionMethod: Sendable {
  public let id: String
  public let grade: ConfidenceGrade
  public let authority: CompletionAuthority
  public let methodName: String
  /// The fence bytes to send behind the payload for a job's four-character token.
  public let fenceBytes: @Sendable (String) -> [UInt8]
  /// Classifies the printer→host bytes accumulated since the last match/not-mine.
  public let matcher: @Sendable ([UInt8]) -> CompletionMatch

  public init(
    id: String,
    grade: ConfidenceGrade = .aJobLevelConfirmation,
    authority: CompletionAuthority = .physicalPrinter,
    methodName: String? = nil,
    fenceBytes: @escaping @Sendable (String) -> [UInt8],
    matcher: @escaping @Sendable ([UInt8]) -> CompletionMatch
  ) {
    self.id = id
    self.grade = grade
    self.authority = authority
    self.methodName = methodName ?? id
    self.fenceBytes = fenceBytes
    self.matcher = matcher
  }
}

/// Holds a ``CompletionMethod`` alive for as long as the driver can call into it, and
/// carries the C function pointers' context — the same retention contract as
/// ``BluetoothTrampoline``.
final class CompletionMethodTrampoline {
  let method: CompletionMethod
  init(_ method: CompletionMethod) { self.method = method }
}

private func completionFenceBytes(
  _ ctx: UnsafeMutableRawPointer?, _ token: UnsafePointer<CChar>?,
  _ out: UnsafeMutablePointer<UInt8>?, _ cap: Int
) -> Int {
  guard let ctx, let out else { return 0 }
  let trampoline = Unmanaged<CompletionMethodTrampoline>.fromOpaque(ctx).takeUnretainedValue()
  let jobToken = token.map { String(cString: $0) } ?? ""
  let bytes = trampoline.method.fenceBytes(jobToken)
  // Over cap is reported as such, never truncated: the core fails the job Unknown.
  if bytes.count > cap { return cap + 1 }
  for (index, byte) in bytes.enumerated() { out[index] = byte }
  return bytes.count
}

private func completionMatcher(
  _ ctx: UnsafeMutableRawPointer?, _ data: UnsafePointer<UInt8>?, _ size: Int
) -> pd_match_result {
  var result = pd_match_result()
  guard let ctx else {
    result.kind = PD_MATCH_NOT_MINE
    return result
  }
  let trampoline = Unmanaged<CompletionMethodTrampoline>.fromOpaque(ctx).takeUnretainedValue()
  let bytes = data.map { Array(UnsafeBufferPointer(start: $0, count: size)) } ?? []
  let verdict = trampoline.method.matcher(bytes)
  result.kind = verdict.cKind
  if case .matched(let token) = verdict {
    let utf8 = Array(token.utf8.prefix(7))
    withUnsafeMutableBytes(of: &result.token) { buffer in
      for (index, byte) in utf8.enumerated() { buffer[index] = byte }
      buffer[utf8.count] = 0
    }
  }
  return result
}

extension PrinterDriver {
  /// Registers a custom completion mechanism (docs/api.md §16). Throws on a bad or
  /// duplicate id, or a record the core rejects.
  public func register(completionMethod method: CompletionMethod) throws {
    let trampoline = CompletionMethodTrampoline(method)
    // Retained before the ABI sees the pointer and never released early, exactly like a
    // custom transport's trampoline: a job may run on a worker thread and call the fence
    // the moment the printer is attached.
    core.retain(trampoline: trampoline)
    let ctx = Unmanaged.passUnretained(trampoline).toOpaque()
    let ok = method.id.withCString { idPointer -> Int32 in
      method.methodName.withCString { namePointer -> Int32 in
        var descriptor = pd_completion_method(
          id: idPointer,
          fence_bytes: completionFenceBytes,
          matcher: completionMatcher,
          ctx: ctx,
          grade: pd_confidence_grade(method.grade.rawValue),
          authority: pd_completion_authority(method.authority.rawValue),
          method_name: namePointer)
        return withUnsafePointer(to: &descriptor) {
          pd_register_completion_method(core.handle, $0)
        }
      }
    }
    if ok == 0 { throw core.lastError() }
  }
}

// MARK: - Probe step

/// What a custom probe step concluded about the answer it was handed.
public struct ProbeFinding: Sendable {
  /// Whether the device replied to this step at all.
  public let answered: Bool
  /// A short classification, surfaced in the findings summary. Truncated at 63 bytes.
  public let label: String

  public init(answered: Bool, label: String) {
    self.answered = answered
    self.label = label
  }
}

/// An extra fingerprinting step for `probe` and ``PrinterDriver/autoDetect(_:)``
/// (docs/api.md §16).
///
/// - Important: ``requestBytes`` MUST be non-printing — no `0x20`–`0x7E` run, no line
///   feed. A printing step is refused at registration rather than at a venue, because
///   auto-detection must never cost somebody a roll of paper.
public struct ProbeStep: Sendable {
  public let id: String
  public let requestBytes: [UInt8]
  public let classify: @Sendable ([UInt8]) -> ProbeFinding

  public init(
    id: String, requestBytes: [UInt8],
    classify: @escaping @Sendable ([UInt8]) -> ProbeFinding
  ) {
    self.id = id
    self.requestBytes = requestBytes
    self.classify = classify
  }
}

final class ProbeStepTrampoline {
  let step: ProbeStep
  init(_ step: ProbeStep) { self.step = step }
}

private func probeClassify(
  _ ctx: UnsafeMutableRawPointer?, _ response: UnsafePointer<UInt8>?, _ size: Int
) -> pd_probe_finding {
  var finding = pd_probe_finding()
  guard let ctx else { return finding }
  let trampoline = Unmanaged<ProbeStepTrampoline>.fromOpaque(ctx).takeUnretainedValue()
  let bytes = response.map { Array(UnsafeBufferPointer(start: $0, count: size)) } ?? []
  let answer = trampoline.step.classify(bytes)
  finding.answered = answer.answered ? 1 : 0
  let utf8 = Array(answer.label.utf8.prefix(63))
  withUnsafeMutableBytes(of: &finding.label) { buffer in
    for (index, byte) in utf8.enumerated() { buffer[index] = byte }
    buffer[utf8.count] = 0
  }
  return finding
}

// MARK: - Document block handler

/// What a custom block handler made of a block: ops, or a declared degradation.
public enum BlockRendering: Sendable {
  /// Raw ESC/POS ops, rendered through the ordinary pipeline.
  case ops([UInt8])
  /// This block could not be drawn, and here is the one line saying so. Reported exactly
  /// like a built-in block's degradation — never dropped in silence.
  case degraded(String)
}

/// Renders a new DSL block kind (docs/api.md §16). A handler registered for a kind always
/// owns it: unknown kinds otherwise degrade, and this intercepts first.
public struct BlockHandler: Sendable {
  /// The block object key that selects this handler.
  public let kind: String
  /// `blockJSON` is the block object; `profileJSON` is a small JSON of the render profile
  /// facts (`{"width_dots":576,"barcode":true,...}`).
  public let render: @Sendable (_ blockJSON: String, _ profileJSON: String) -> BlockRendering

  public init(
    kind: String,
    render: @escaping @Sendable (_ blockJSON: String, _ profileJSON: String) -> BlockRendering
  ) {
    self.kind = kind
    self.render = render
  }
}

final class BlockHandlerTrampoline {
  let handler: BlockHandler
  init(_ handler: BlockHandler) { self.handler = handler }
}

private func blockRender(
  _ ctx: UnsafeMutableRawPointer?, _ blockJSON: UnsafePointer<CChar>?,
  _ profileJSON: UnsafePointer<CChar>?, _ out: UnsafeMutablePointer<UInt8>?, _ cap: Int,
  _ ok: UnsafeMutablePointer<Int32>?, _ detail: UnsafeMutablePointer<CChar>?,
  _ detailCap: Int
) -> Int {
  guard let ctx, let out, let ok else { return 0 }
  let trampoline = Unmanaged<BlockHandlerTrampoline>.fromOpaque(ctx).takeUnretainedValue()
  let block = blockJSON.map { String(cString: $0) } ?? ""
  let profile = profileJSON.map { String(cString: $0) } ?? ""
  switch trampoline.handler.render(block, profile) {
  case .ops(let bytes):
    ok.pointee = 1
    // Over cap is an error, never a truncation: half a block is not a receipt.
    if bytes.count > cap { return cap + 1 }
    for (index, byte) in bytes.enumerated() { out[index] = byte }
    return bytes.count
  case .degraded(let reason):
    ok.pointee = 0
    if let detail, detailCap > 0 {
      let utf8 = Array(reason.utf8.prefix(detailCap - 1))
      for (index, byte) in utf8.enumerated() { detail[index] = CChar(bitPattern: byte) }
      detail[utf8.count] = 0
    }
    return 0
  }
}

// MARK: - Template formatter

/// Backs `{{ v | name:args }}` in the template layer, checked before the built-in table
/// (docs/api.md §16). Returning `nil` declines and falls through to the built-ins.
public struct TemplateFormatter: Sendable {
  public let name: String
  public let format: @Sendable (_ value: String, _ args: String, _ locale: String) -> String?

  public init(
    name: String,
    format: @escaping @Sendable (_ value: String, _ args: String, _ locale: String) -> String?
  ) {
    self.name = name
    self.format = format
  }
}

final class FormatterTrampoline {
  let formatter: TemplateFormatter
  init(_ formatter: TemplateFormatter) { self.formatter = formatter }
}

private func formatValue(
  _ ctx: UnsafeMutableRawPointer?, _ value: UnsafePointer<CChar>?,
  _ args: UnsafePointer<CChar>?, _ locale: UnsafePointer<CChar>?,
  _ out: UnsafeMutablePointer<CChar>?, _ cap: Int, _ handled: UnsafeMutablePointer<Int32>?
) -> Int {
  guard let ctx, let out, let handled else { return 0 }
  let trampoline = Unmanaged<FormatterTrampoline>.fromOpaque(ctx).takeUnretainedValue()
  let text = trampoline.formatter.format(
    value.map { String(cString: $0) } ?? "",
    args.map { String(cString: $0) } ?? "",
    locale.map { String(cString: $0) } ?? "")
  guard let text else {
    handled.pointee = 0
    return 0
  }
  handled.pointee = 1
  let utf8 = Array(text.utf8)
  if utf8.count > cap { return cap + 1 }
  for (index, byte) in utf8.enumerated() { out[index] = CChar(bitPattern: byte) }
  return utf8.count
}

// MARK: - Drawer kick method

/// A vendor drawer-kick method, filling ``DrawerKickMethod/vendor`` for a profile
/// (docs/api.md §16, docs/cash-drawer.md).
///
/// ``statusRequest`` and ``statusParse`` go together: supplying neither means the method
/// has no readable switch, so a kick reports ``DrawerState/kickSentUnverified`` rather
/// than a verified open. That is the honest answer, not a weak success.
public struct DrawerKick: Sendable {
  public let id: String
  /// The pulse bytes for a channel and duration.
  public let kickBytes: @Sendable (_ channel: UInt8, _ pulseMilliseconds: UInt16) -> [UInt8]
  /// The bytes that ask for the switch state.
  public let statusRequest: (@Sendable () -> [UInt8])?
  /// The pin level a reply carries: `true` high, `false` low, `nil` unreadable.
  public let statusParse: (@Sendable ([UInt8]) -> Bool?)?

  public init(
    id: String,
    kickBytes: @escaping @Sendable (_ channel: UInt8, _ pulseMilliseconds: UInt16) -> [UInt8],
    statusRequest: (@Sendable () -> [UInt8])? = nil,
    statusParse: (@Sendable ([UInt8]) -> Bool?)? = nil
  ) {
    self.id = id
    self.kickBytes = kickBytes
    self.statusRequest = statusRequest
    self.statusParse = statusParse
  }
}

final class DrawerKickTrampoline {
  let kick: DrawerKick
  init(_ kick: DrawerKick) { self.kick = kick }
}

private func drawerKickBytes(
  _ ctx: UnsafeMutableRawPointer?, _ channel: UInt8, _ pulseMs: UInt16,
  _ out: UnsafeMutablePointer<UInt8>?, _ cap: Int
) -> Int {
  guard let ctx, let out else { return 0 }
  let trampoline = Unmanaged<DrawerKickTrampoline>.fromOpaque(ctx).takeUnretainedValue()
  let bytes = trampoline.kick.kickBytes(channel, pulseMs)
  if bytes.count > cap { return cap + 1 }
  for (index, byte) in bytes.enumerated() { out[index] = byte }
  return bytes.count
}

private func drawerStatusRequest(
  _ ctx: UnsafeMutableRawPointer?, _ out: UnsafeMutablePointer<UInt8>?, _ cap: Int
) -> Int {
  guard let ctx, let out else { return 0 }
  let trampoline = Unmanaged<DrawerKickTrampoline>.fromOpaque(ctx).takeUnretainedValue()
  guard let request = trampoline.kick.statusRequest else { return 0 }
  let bytes = request()
  if bytes.count > cap { return cap + 1 }
  for (index, byte) in bytes.enumerated() { out[index] = byte }
  return bytes.count
}

private func drawerStatusParse(
  _ ctx: UnsafeMutableRawPointer?, _ response: UnsafePointer<UInt8>?, _ size: Int
) -> Int32 {
  guard let ctx else { return Int32(PD_UNKNOWN) }
  let trampoline = Unmanaged<DrawerKickTrampoline>.fromOpaque(ctx).takeUnretainedValue()
  guard let parse = trampoline.kick.statusParse else { return Int32(PD_UNKNOWN) }
  let bytes = response.map { Array(UnsafeBufferPointer(start: $0, count: size)) } ?? []
  guard let level = parse(bytes) else { return Int32(PD_UNKNOWN) }
  return level ? Int32(PD_TRUE) : Int32(PD_FALSE)
}

// MARK: - Registration

extension PrinterDriver {
  /// Registers an extra fingerprinting step for `probe` and auto-detection
  /// (docs/api.md §16). Throws on a bad or duplicate id — and on a request whose bytes
  /// could print, which the core refuses at registration.
  public func register(probeStep step: ProbeStep) throws {
    let trampoline = ProbeStepTrampoline(step)
    core.retain(trampoline: trampoline)
    let ctx = Unmanaged.passUnretained(trampoline).toOpaque()
    let ok = step.id.withCString { idPointer -> Int32 in
      step.requestBytes.withUnsafeBufferPointer { request -> Int32 in
        var descriptor = pd_probe_step(
          id: idPointer,
          request_bytes: request.baseAddress,
          request_size: request.count,
          classify: probeClassify,
          ctx: ctx)
        return withUnsafePointer(to: &descriptor) { pd_register_probe_step(core.handle, $0) }
      }
    }
    if ok == 0 { throw core.lastError() }
  }

  /// Registers a renderer for a new DSL block kind (docs/api.md §16).
  ///
  /// The handler always owns its kind: an unknown block kind otherwise fails the parse,
  /// and this intercepts first.
  ///
  /// Reached through ``Printer/renderDocument(_:model:options:)`` and
  /// ``Printer/printDocument(_:model:options:)`` — the receipt-DSL entry points — not
  /// through ``Printer/print(_:options:)``, whose payload tiers have no block kinds.
  public func register(blockHandler handler: BlockHandler) throws {
    let trampoline = BlockHandlerTrampoline(handler)
    core.retain(trampoline: trampoline)
    let ctx = Unmanaged.passUnretained(trampoline).toOpaque()
    let ok = handler.kind.withCString { kindPointer -> Int32 in
      var descriptor = pd_block_handler(kind: kindPointer, handler: blockRender, ctx: ctx)
      return withUnsafePointer(to: &descriptor) { pd_register_block_handler(core.handle, $0) }
    }
    if ok == 0 { throw core.lastError() }
  }

  /// Registers a template formatter (docs/api.md §16). Checked before the built-in table,
  /// so a name that shadows a built-in wins for this driver.
  ///
  /// Consulted wherever a template is bound: ``Printer/renderDocument(_:model:options:)``,
  /// ``Printer/printDocument(_:model:options:)`` and this driver's self-test tickets.
  public func register(formatter: TemplateFormatter) throws {
    let trampoline = FormatterTrampoline(formatter)
    core.retain(trampoline: trampoline)
    let ctx = Unmanaged.passUnretained(trampoline).toOpaque()
    let ok = formatter.name.withCString { namePointer -> Int32 in
      var descriptor = pd_formatter(name: namePointer, formatter: formatValue, ctx: ctx)
      return withUnsafePointer(to: &descriptor) { pd_register_formatter(core.handle, $0) }
    }
    if ok == 0 { throw core.lastError() }
  }

  /// Registers a vendor drawer-kick method (docs/api.md §16).
  public func register(drawerKick kick: DrawerKick) throws {
    let trampoline = DrawerKickTrampoline(kick)
    core.retain(trampoline: trampoline)
    let ctx = Unmanaged.passUnretained(trampoline).toOpaque()
    let ok = kick.id.withCString { idPointer -> Int32 in
      var descriptor = pd_drawer_kick_reg(
        id: idPointer,
        kick_bytes: drawerKickBytes,
        status_request: kick.statusRequest == nil ? nil : drawerStatusRequest,
        status_parse: kick.statusParse == nil ? nil : drawerStatusParse,
        ctx: ctx)
      return withUnsafePointer(to: &descriptor) { pd_register_drawer_kick(core.handle, $0) }
    }
    if ok == 0 { throw core.lastError() }
  }
}
