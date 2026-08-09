import CPrinterDriver

// The enums are the part of this wrapper that is allowed to know anything, and even here
// the knowledge is only "which C constant is which Swift case". Every type below mirrors
// one `pd_*` enum from capi/include/printerdriver/pd.h member for member, in declaration
// order, with the same numeric values. pd.h in turn is static_asserted against the C++
// core in capi/src/pd_capi.cpp, so a member added to the core and forgotten here is
// caught by PrinterDriverTests.EnumBridgeTests rather than by a customer.

// MARK: - Bridging

/// A Swift enum that mirrors a closed C enum from `pd.h`.
///
/// The ABI's enums are closed by design (docs/api.md §1.3): a wrapper may not add, drop
/// or partially implement values. That makes every mirror total *for the header it was
/// built against* — but a Swift binary can outlive the C library it links, so the bridge
/// still has to answer the question "what if the core hands me a value I have never
/// heard of?".
///
/// The answer is deliberately loud and never lossy:
///
/// * **Debug builds trap.** An unmapped value means this wrapper is out of date with the
///   core it is linked against, which is a build-configuration bug, and it should stop
///   the process where it happens rather than at the till.
/// * **Release builds substitute ``unrecognizedFallback``** — a real member of the enum,
///   chosen per type so the substitution can only ever understate what is known
///   (see each type's documentation). The value is still delivered to the app: nothing
///   is dropped, no stream is truncated, no callback is skipped.
public protocol ABIMirroredEnum: RawRepresentable, CaseIterable, Hashable, Sendable
where RawValue == UInt32 {
  /// The spelling used in diagnostics; matches the C++ core's own type name.
  static var abiTypeName: String { get }

  /// The member substituted for an unrecognized ABI value in release builds.
  static var unrecognizedFallback: Self { get }
}

extension ABIMirroredEnum {
  /// Maps a raw ABI value onto this enum, trapping in debug and falling back in release.
  ///
  /// - Note: This is the only entry point through which C values become Swift values.
  ///   Nothing in this package constructs a mirrored enum from an integer any other way.
  public init(bridging rawValue: UInt32) {
    if let mapped = Self(rawValue: rawValue) {
      self = mapped
      return
    }
    assertionFailure(
      """
      PrinterDriver: the core produced \(Self.abiTypeName) value \(rawValue), which this \
      wrapper does not know. The Swift wrapper is older than the linked core; regenerate \
      it from capi/include/printerdriver/pd.h. Falling back to \
      \(Self.unrecognizedFallback) in release builds.
      """
    )
    self = Self.unrecognizedFallback
  }
}

// MARK: - JobState

/// Where a job is in the lifecycle of `docs/techspec.md` §5.1 — `pd_job_state`.
public enum JobState: UInt32, ABIMirroredEnum {
  case queued = 0
  case preflightOk = 1
  case sendStarted = 2
  case bytesSent = 3
  case printConfirmed = 4
  case cutCommandProcessed = 5
  case doneSoftware = 6
  /// Reserved for hardware verification (exit sensor, QR scan); never produced today.
  case physicallyVerified = 7
  case failedKnown = 8
  /// Sent, never acknowledged. Not a success and not a failure — an operator decision.
  case unknown = 9
  /// Produced only by the print-queue addon (docs/sdk-spec.md §12).
  case heldOffline = 10

  public static let abiTypeName = "JobState"
  /// An unknown state is, precisely, unknown — the one honest substitution available.
  public static let unrecognizedFallback = JobState.unknown

  /// The core's own spelling, from `pd_job_state_name`.
  public var abiName: String { String(cString: pd_job_state_name(pd_job_state(rawValue))) }
}

// MARK: - ConfidenceLevel

/// What evidence backs a claim about a job — `pd_confidence_level`.
///
/// The ladder only ever climbs on evidence the printer actually produced; the SDK never
/// inflates it, and neither does this wrapper.
public enum ConfidenceLevel: UInt32, ABIMirroredEnum {
  /// Bytes reached a buffer somewhere. The floor.
  case transportAccepted = 0
  case printerHealthy = 1
  case printConfirmed = 2
  case cutProcessed = 3
  case cutFaultFree = 4
  case physicallyVerified = 5

  public static let abiTypeName = "ConfidenceLevel"
  /// The bottom rung. An unrecognized level must never be read as *more* evidence than
  /// the wrapper can account for.
  public static let unrecognizedFallback = ConfidenceLevel.transportAccepted

  /// The core's own spelling, from `pd_confidence_level_name`.
  public var abiName: String {
    String(cString: pd_confidence_level_name(pd_confidence_level(rawValue)))
  }
}

// MARK: - ConfidenceGrade

/// What *class* of evidence a claim rests on — `pd_confidence_grade`.
///
/// Orthogonal to ``ConfidenceLevel``: the level says how far up the evidence ladder a
/// job climbed, the grade says what the claim is made of. A job that is done at
/// ``ConfidenceLevel/cutProcessed`` on grade A and one that is done at
/// ``ConfidenceLevel/cutProcessed`` on grade D are not making the same claim.
public enum ConfidenceGrade: UInt32, ABIMirroredEnum {
  /// A durable, queryable printer-side job: ePOS submits, returns a JobID, and the result
  /// is retrievable afterwards — the only mechanism in the hierarchy that survives the
  /// app losing its connection between submission and answer
  /// (docs/compatibility-brief.md §24).
  ///
  /// - Important: **Nothing produces this grade yet.** The ePOS transport does not exist
  ///   in the core, so no ``JobResult`` can carry it. It is declared now because the ABI's
  ///   enums are closed and mirrored by four wrappers, and adding a member later would
  ///   renumber every mirror a second time. A profile that reports an ePOS JobID is
  ///   describing hardware, not making a claim, and is graded ``aJobLevelConfirmation``.
  case aPlusDurableQueryableJob = 0
  /// `GS ( H`, a Star checked block, a documented vendor equivalent.
  case aJobLevelConfirmation = 1
  /// `GS r`, a vendor idle query.
  case bOrderedDeviceResponse = 2
  /// `DLE EOT`, ASB or SNMP taken around the transmission.
  case cDeviceStatusAround = 3
  /// A spooler or IPP gateway said completed.
  case dSpoolerCompleted = 4
  /// The write succeeded and nothing else is known.
  case eTransportOnly = 5

  public static let abiTypeName = "ConfidenceGrade"
  /// The weakest grade there is. An unrecognized one must never read as stronger
  /// evidence than the wrapper can account for.
  public static let unrecognizedFallback = ConfidenceGrade.eTransportOnly

  /// The core's own spelling, from `pd_confidence_grade_name`.
  public var abiName: String {
    String(cString: pd_confidence_grade_name(pd_confidence_grade(rawValue)))
  }

  /// `"A+"`, `"A"`…`"E"` — the letter a report tabulates, from
  /// `pd_confidence_grade_letter`.
  public var letter: String {
    String(cString: pd_confidence_grade_letter(pd_confidence_grade(rawValue)))
  }
}

// MARK: - Provenance

/// Where the claim that a printer has a capability comes from —
/// `pd_provenance`, docs/compatibility-brief.md §28.
///
/// Recognising ESC/POS *print* commands does not prove the Epson *feedback* extensions,
/// which is the mistake the whole compatibility brief is arranged around. So "the
/// manufacturer's manual says so", "we asked the hardware and it answered" and "nobody
/// has checked" are three answers rather than one boolean.
///
/// The three are independent, not ordered. A probe can contradict documentation when the
/// interface path swallows responses; documentation can cover a model no probe has
/// reached. `pdctl probe` prints both columns side by side for exactly that reason.
public enum Provenance: UInt32, ABIMirroredEnum {
  /// The manufacturer's own command documentation lists it for this model. Epson is the
  /// only family in the shipped database whose defaults ever claim this.
  case documented = 0
  /// This driver asked the installed hardware over the installed interface path and it
  /// answered. Specific to the path it was measured on — and stronger than marketing.
  case probed = 1
  /// Neither. A shipped default nobody has confirmed, which is what "ESC/POS compatible"
  /// on a datasheet amounts to.
  case unverified = 2

  public static let abiTypeName = "Provenance"
  /// Claims the least: an unrecognized provenance has established nothing.
  public static let unrecognizedFallback = Provenance.unverified

  /// The core's own spelling, from `pd_provenance_name`.
  public var abiName: String { String(cString: pd_provenance_name(pd_provenance(rawValue))) }
}

// MARK: - CommandLanguage

/// What a device actually speaks — `pd_command_language`,
/// docs/compatibility-brief.md §1.
///
/// Only ``escPos`` is implemented. The rest exist so a fleet containing them can be
/// described rather than misdriven: a profile naming ZPL, CPCL, Brother raster or ESC/P
/// is refused with ``FailureReason/unsupported`` before a byte is written, because an
/// ESC/POS engine pointed at a Zebra prints a metre of text instead of a label.
public enum CommandLanguage: UInt32, ABIMirroredEnum {
  case escPos = 0
  case starPrnt = 1
  case starLine = 2
  case eposXml = 3
  /// Zebra Programming Language (Link-OS).
  case zpl = 4
  /// Comtec/Zebra mobile, also documented by the Citizen CMP portables.
  case cpcl = 5
  case brotherRaster = 6
  /// Brother ESC/P — a different language from Epson ESC/POS, despite the name.
  case escP = 7

  public static let abiTypeName = "CommandLanguage"
  /// Not a safe default in the usual sense, but the only member this core can drive; an
  /// unrecognized language is caught by the debug trap in ``ABIMirroredEnum``.
  public static let unrecognizedFallback = CommandLanguage.escPos

  /// The core's own spelling, from `pd_command_language_name`.
  public var abiName: String {
    String(cString: pd_command_language_name(pd_command_language(rawValue)))
  }
}

// MARK: - DeviceEvent

/// The per-printer stream that replaces availability polling — `pd_device_event`.
public enum DeviceEvent: UInt32, ABIMirroredEnum {
  case online = 0
  case offline = 1
  case coverOpen = 2
  case coverClosed = 3
  case paperOut = 4
  case paperNearEnd = 5
  case paperOk = 6
  case cutterError = 7
  case recoverableError = 8
  case unrecoverableError = 9
  case connectionLost = 10
  case connectionRestored = 11
  /// A `GS ( H` echo arrived carrying a token this driver never issued: something else
  /// is writing to the same printer (docs/api.md §14). The echo is attributed to no job,
  /// so no fence is satisfied by it — one printer has exactly one connection owner, and
  /// this is what a violation of that looks like from the inside.
  case foreignWriterDetected = 12

  public static let abiTypeName = "DeviceEvent"
  /// This enum has no "unknown" member, so the fallback is chosen for least harm: an
  /// unrecognized event stays visible to the operator without asserting a device state
  /// nobody reported (`offline`, `paperOut`, …) and without escalating to the
  /// unrecoverable bucket that stops a line.
  public static let unrecognizedFallback = DeviceEvent.recoverableError

  /// The core's own spelling, from `pd_device_event_name`.
  public var abiName: String {
    String(cString: pd_device_event_name(pd_device_event(rawValue)))
  }
}

// MARK: - FailureReason

/// Why a job failed — `pd_failure_reason`.
public enum FailureReason: UInt32, ABIMirroredEnum {
  case none = 0
  case transportUnreachable = 1
  case preflightCoverOpen = 2
  case preflightPaperOut = 3
  case preflightHardwareError = 4
  /// The completion wait ran out. Note this ends a job in ``JobResult/unknown(confidence:)``,
  /// not in a failure: bytes were sent and the receipt may well be printing.
  case timeoutAwaitingCompletion = 5
  case cutterFault = 6
  case unsupported = 7
  case unknown = 8
  /// Print-queue addon only (docs/sdk-spec.md §12).
  case expired = 9
  /// Print-queue addon only (docs/sdk-spec.md §12).
  case queueOverflow = 10

  public static let abiTypeName = "FailureReason"
  public static let unrecognizedFallback = FailureReason.unknown

  /// The core's own spelling, from `pd_failure_reason_name`.
  public var abiName: String {
    String(cString: pd_failure_reason_name(pd_failure_reason(rawValue)))
  }
}

// MARK: - JobOutcome

/// The tri-state discriminant of `pd_job_result`.
///
/// Bound as an enum on purpose. Apps consume the sealed ``JobResult`` instead, which
/// carries this discriminant's payload; this type exists so the ABI's own three-way
/// split stays visible and testable.
public enum JobOutcome: UInt32, ABIMirroredEnum {
  case done = 0
  case failed = 1
  case unknown = 2

  public static let abiTypeName = "JobOutcome"
  public static let unrecognizedFallback = JobOutcome.unknown

  /// The core's own spelling, from `pd_job_outcome_name`.
  public var abiName: String {
    String(cString: pd_job_outcome_name(pd_job_outcome(rawValue)))
  }
}

// MARK: - Cut

/// How a job ends the paper — `pd_cut`.
public enum Cut: UInt32, ABIMirroredEnum {
  /// Whatever this printer's cutter does. The default, and the zero value of the ABI.
  case profile = 0
  case partial = 1
  case full = 2
  case none = 3

  public static let abiTypeName = "CutSetting"
  public static let unrecognizedFallback = Cut.profile
}

// MARK: - Preflight

/// Whether a job refuses to start on a device that already reports a fault — `pd_preflight`.
public enum Preflight: UInt32, ABIMirroredEnum {
  case strict = 0
  case skip = 1

  public static let abiTypeName = "PreflightMode"
  public static let unrecognizedFallback = Preflight.strict
}

// MARK: - PayloadKind

/// The tier a job was submitted as — `pd_payload_kind`.
public enum PayloadKind: UInt32, ABIMirroredEnum {
  case raster = 0
  case document = 1
  case raw = 2

  public static let abiTypeName = "PayloadKind"
  public static let unrecognizedFallback = PayloadKind.raw

  /// The core's own spelling, from `pd_payload_kind_name`.
  public var abiName: String {
    String(cString: pd_payload_kind_name(pd_payload_kind(rawValue)))
  }
}

// MARK: - CompletionMechanism

/// Which ordered fence a printer's profile answers — `pd_completion_mechanism`.
///
/// This is what a `done` claim rests on at the protocol level: a `GS ( H` printer can
/// prove the cut was processed fault-free, a `GS r 1` printer can prove ordering only,
/// and a `none` printer proves nothing beyond the transport accepting bytes.
public enum CompletionMechanism: UInt32, ABIMirroredEnum {
  case gsParenH = 0
  case gsR1 = 1
  case vendorIdle = 2
  case eposJobId = 3
  case starCheckedBlock = 4
  case none = 5

  public static let abiTypeName = "CompletionMechanism"
  /// Claims nothing. An unrecognized mechanism must not imply a fence the wrapper cannot
  /// describe.
  public static let unrecognizedFallback = CompletionMechanism.none

  /// The core's own spelling, from `pd_completion_mechanism_name`.
  public var abiName: String {
    String(cString: pd_completion_mechanism_name(pd_completion_mechanism(rawValue)))
  }
}

// MARK: - CompletionAuthority

/// Who is actually making the claim a ``JobResult`` carries — `pd_completion_authority`.
///
/// Recorded separately from ``ConfidenceGrade`` because "completed" from a print server
/// and "completed" from the mechanism that moved the paper are different facts.
public enum CompletionAuthority: UInt32, ABIMirroredEnum {
  /// The device that moved the paper said so itself.
  case physicalPrinter = 0
  case vendorSpooler = 1
  case pdAgent = 2
  case printServer = 3
  /// Nobody said anything; the write succeeded.
  case transportOnly = 4

  public static let abiTypeName = "CompletionAuthority"
  /// Claims the least. An unrecognized authority must not be read as the printer.
  public static let unrecognizedFallback = CompletionAuthority.transportOnly

  /// The core's own spelling, from `pd_completion_authority_name`.
  public var abiName: String {
    String(cString: pd_completion_authority_name(pd_completion_authority(rawValue)))
  }
}

// MARK: - CutVariant

/// The cut a profile's mechanism actually performs — `pd_cut_variant`.
public enum CutVariant: UInt32, ABIMirroredEnum {
  case partial = 0
  case full = 1
  case none = 2

  public static let abiTypeName = "CutVariant"
  public static let unrecognizedFallback = CutVariant.none

  /// The core's own spelling, from `pd_cut_variant_name`.
  public var abiName: String {
    String(cString: pd_cut_variant_name(pd_cut_variant(rawValue)))
  }
}

// MARK: - Alignment

/// Document-tier alignment — `pd_alignment`. Values are the `n` operand of `ESC a n`.
public enum Alignment: UInt32, ABIMirroredEnum {
  case left = 0
  case center = 1
  case right = 2

  public static let abiTypeName = "Alignment"
  public static let unrecognizedFallback = Alignment.left
}

// MARK: - CodePage

/// Document-tier code page — `pd_code_page`.
///
/// Values are the `n` operand of `ESC t n`, so they are **not** contiguous and
/// `PD_CODE_PAGE_COUNT` is a member count rather than a past-the-end sentinel. Iterate
/// ``allCases`` (or the ABI's `pd_code_page_at`), never `0..<count`.
public enum CodePage: UInt32, ABIMirroredEnum {
  case pc437 = 0
  case pc850 = 2
  case wpc1252 = 16
  case pc852 = 18
  case pc858 = 19

  public static let abiTypeName = "CodePage"
  public static let unrecognizedFallback = CodePage.pc437
}

// MARK: - Binarization

/// How the raster tier turns grey into dots — `pd_binarization`.
public enum Binarization: UInt32, ABIMirroredEnum {
  case fixedThreshold = 0
  case floydSteinberg = 1

  public static let abiTypeName = "Binarization"
  public static let unrecognizedFallback = Binarization.fixedThreshold
}

// MARK: - M14: cash drawer (docs/cash-drawer.md)

/// What is known about a cash drawer — `pd_drawer_state`.
///
/// The drawer document rejects `{sent: true}` in its first paragraph, and this enum is
/// why: sending `ESC p` and watching the microswitch change are different claims, and an
/// operator standing at a till needs them apart. ``kickSentUnverified`` is a real answer
/// rather than a softer success — through a cheap print server the pulse travels forward
/// while the sensor response never comes back.
public enum DrawerState: UInt32, ABIMirroredEnum {
  case closed = 0
  case open = 1
  case opening = 2
  case kickSentUnverified = 3
  /// The only case that claims physical movement.
  case openVerified = 4
  case failedToOpen = 5
  /// This port has no switch input wired or documented. Not an error.
  case noSensor = 6
  case unknown = 7

  public static let abiTypeName = "DrawerState"
  /// Understates by construction: an unrecognized state claims nothing at all, which is
  /// the only substitution that cannot turn an unconfirmed pulse into a confirmed one.
  public static let unrecognizedFallback = DrawerState.unknown

  /// The core's own spelling, from `pd_drawer_state_name`.
  public var abiName: String {
    String(cString: pd_drawer_state_name(pd_drawer_state(rawValue)))
  }
}

/// The electrical classification of a drawer port — `pd_drawer_port_standard`.
///
/// RJ11/RJ12-looking drawer connectors are not a universal electrical standard. Star's
/// identical-looking 6P6C socket carries +24 V on pin 3 and the sense line on pin 6,
/// precisely where Epson puts sense and signal ground, which is why printer-specific
/// drawer cables exist for plugs that look the same.
public enum DrawerPortStandard: UInt32, ABIMirroredEnum {
  /// 1 FG, 2 kick 1, 3 sensor, 4 +24 V, 5 kick 2, 6 signal ground. Epson, Citizen, much
  /// of Bixolon and many clones: one large interoperable ecosystem.
  case epson24V6P6C = 0
  case star24V6P6C = 1
  case generic12V6P6C = 2
  case unknown = 3

  public static let abiTypeName = "DrawerPortStandard"
  /// Nothing is fired on an unknown port, so this fallback can only ever refuse.
  public static let unrecognizedFallback = DrawerPortStandard.unknown

  /// The core's own spelling, from `pd_drawer_port_standard_name`.
  public var abiName: String {
    String(cString: pd_drawer_port_standard_name(pd_drawer_port_standard(rawValue)))
  }
}

/// Which software path fires the drawer — `pd_drawer_kick_method`.
///
/// Independent of ``DrawerPortStandard``: two printers accepting the same "kick drawer 1"
/// command may still wire the modular socket differently.
public enum DrawerKickMethod: UInt32, ABIMirroredEnum {
  case epsonEscP = 0
  case epsonEpos = 1
  case starPrnt = 2
  case bixolonSdk = 3
  case citizenEscP = 4
  case snbcEscP = 5
  case vendor = 6
  case unsupported = 7

  public static let abiTypeName = "DrawerKickMethod"
  public static let unrecognizedFallback = DrawerKickMethod.unsupported

  /// The core's own spelling, from `pd_drawer_kick_method_name`.
  public var abiName: String {
    String(cString: pd_drawer_kick_method_name(pd_drawer_kick_method(rawValue)))
  }
}

/// How the drawer switch is read back — `pd_drawer_status_method`.
public enum DrawerStatusMethod: UInt32, ABIMirroredEnum {
  case gsR2 = 0
  case asb = 1
  case starSignal = 2
  case vendorSdk = 3
  case none = 4

  public static let abiTypeName = "DrawerStatusMethod"
  public static let unrecognizedFallback = DrawerStatusMethod.none

  /// The core's own spelling, from `pd_drawer_status_method_name`.
  public var abiName: String {
    String(cString: pd_drawer_status_method_name(pd_drawer_status_method(rawValue)))
  }
}
