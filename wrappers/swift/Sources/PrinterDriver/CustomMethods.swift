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
  switch trampoline.method.matcher(bytes) {
  case .matched(let token):
    result.kind = PD_MATCH_MATCHED
    let utf8 = Array(token.utf8.prefix(7))
    withUnsafeMutableBytes(of: &result.token) { buffer in
      for (index, byte) in utf8.enumerated() { buffer[index] = byte }
      buffer[utf8.count] = 0
    }
  case .notMine:
    result.kind = PD_MATCH_NOT_MINE
  case .needMore:
    result.kind = PD_MATCH_NEED_MORE
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
