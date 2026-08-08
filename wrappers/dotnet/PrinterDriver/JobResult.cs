namespace PrinterDriver;

/// <summary>
/// The terminal answer for a job: <see cref="Done"/>, <see cref="Failed"/> or
/// <see cref="Unknown"/>.
/// </summary>
/// <remarks>
/// <para>
/// There is no success boolean anywhere in this type, and that is the whole point
/// (docs/api.md §1.4). Collapsing <see cref="Unknown"/> into either bucket is the bug
/// that produces duplicate kitchen tickets — treat it as failed and you reprint a receipt
/// that already came out; treat it as done and you silently drop one. The hierarchy is
/// closed: the constructor is private, so the only three subtypes that can ever exist are
/// the three nested here, and a <c>switch</c> that handles all three handles every case
/// the SDK can produce.
/// </para>
/// <para>
/// <see cref="Confidence"/> is carried on all three outcomes, not only on
/// <see cref="Done"/>. On a failure or an unknown it records how far up the evidence
/// ladder the job got before it stopped, which is what an operator needs in order to
/// decide about a reprint.
/// </para>
/// </remarks>
public abstract record JobResult
{
    private JobResult(ConfidenceLevel confidence, FailureReason reason)
    {
        Confidence = confidence;
        Reason = reason;
    }

    /// <summary>How far up the evidence ladder the job got.</summary>
    public ConfidenceLevel Confidence { get; }

    /// <summary>
    /// Why it failed, or why the outcome is unknown. <see cref="FailureReason.None"/> on
    /// <see cref="Done"/>.
    /// </summary>
    public FailureReason Reason { get; }

    /// <summary>It printed, with the confidence recorded.</summary>
    /// <param name="Confidence">Evidence backing the claim.</param>
    /// <param name="Reason">Always <see cref="FailureReason.None"/>.</param>
    public sealed record Done(ConfidenceLevel Confidence, FailureReason Reason)
        : JobResult(Confidence, Reason);

    /// <summary>
    /// It did not print, and the SDK knows that. Resubmitting the same idempotency key is
    /// safe.
    /// </summary>
    /// <param name="Confidence">Evidence backing the claim.</param>
    /// <param name="Reason">What went wrong.</param>
    public sealed record Failed(ConfidenceLevel Confidence, FailureReason Reason)
        : JobResult(Confidence, Reason);

    /// <summary>
    /// It may or may not have printed. Resubmitting the same key does nothing;
    /// <see cref="Printer.ForceReprint"/> is the deliberate duplicate, and it prints a
    /// reprint banner.
    /// </summary>
    /// <param name="Confidence">Evidence backing the claim.</param>
    /// <param name="Reason">What is known about why it stalled.</param>
    public sealed record Unknown(ConfidenceLevel Confidence, FailureReason Reason)
        : JobResult(Confidence, Reason);

    internal static JobResult FromNative(in PdJobResult native)
    {
        var confidence = (ConfidenceLevel)native.Confidence;
        var reason = (FailureReason)native.Reason;
        return (JobOutcome)native.Outcome switch
        {
            JobOutcome.Done => new Done(confidence, reason),
            JobOutcome.Failed => new Failed(confidence, reason),
            JobOutcome.Unknown => new Unknown(confidence, reason),
            // Not reachable through the ABI, which is closed and asserted against the
            // core at compile time. If it ever happens, the safe reading of an outcome
            // this wrapper does not recognise is "we do not know what happened" -- never
            // "it worked".
            _ => new Unknown(confidence, reason),
        };
    }
}
