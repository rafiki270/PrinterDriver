using System.Reflection;

namespace PrinterDriver.Tests;

/// <summary>
/// The tri-state contract, asserted structurally rather than by reading the source.
/// </summary>
/// <remarks>
/// docs/api.md §1.4: there is no success boolean anywhere in this ABI, and collapsing
/// Unknown into either bucket is the bug that produces duplicate kitchen tickets. These
/// tests exist so that adding an <c>IsSuccess</c> convenience — the exact well-meant
/// change that would undo the design — fails a build rather than shipping.
/// </remarks>
public sealed class JobResultTests
{
    [Fact]
    public void TheHierarchyIsExactlyThreeCasesAndIsClosed()
    {
        var cases = typeof(JobResult).GetNestedTypes(BindingFlags.Public)
            .Where(t => t.IsSubclassOf(typeof(JobResult)))
            .ToList();

        Assert.Equal(3, cases.Count);
        Assert.All(cases, t => Assert.True(t.IsSealed, $"{t.Name} must be sealed"));
        Assert.Contains(typeof(JobResult.Done), cases);
        Assert.Contains(typeof(JobResult.Failed), cases);
        Assert.Contains(typeof(JobResult.Unknown), cases);

        // Closed: every constructor of the base is private, so no assembly — including
        // this one — can add a fourth case.
        var constructors = typeof(JobResult).GetConstructors(
            BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
        Assert.All(constructors,
                   c => Assert.True(c.IsPrivate || c.IsFamily,
                                    "JobResult must not expose a public constructor"));
    }

    [Fact]
    public void NoOutcomeIsExposedAsABoolean()
    {
        var booleanMembers = typeof(JobResult)
            .GetMembers(BindingFlags.Public | BindingFlags.Instance | BindingFlags.Static)
            .Select(member => member switch
            {
                PropertyInfo property => (member.Name, Type: property.PropertyType),
                FieldInfo field => (member.Name, Type: field.FieldType),
                MethodInfo method when method.Name.StartsWith("get_", StringComparison.Ordinal)
                    => (member.Name, Type: method.ReturnType),
                _ => (member.Name, Type: typeof(void)),
            })
            .Where(entry => entry.Type == typeof(bool))
            // Equals(object) and friends are value semantics, not outcome semantics.
            .Where(entry => !entry.Name.Contains("Equals", StringComparison.Ordinal))
            .Select(entry => entry.Name)
            .ToList();

        Assert.True(booleanMembers.Count == 0,
                    "JobResult grew a boolean: " + string.Join(", ", booleanMembers) +
                    ". Done/Failed/Unknown is the contract; a bool cannot carry three states.");
    }

    [Fact]
    public void EveryCaseCarriesTheConfidenceItReached()
    {
        JobResult[] all =
        [
            new JobResult.Done(ConfidenceLevel.CutFaultFree, FailureReason.None),
            new JobResult.Failed(ConfidenceLevel.PrinterHealthy, FailureReason.CutterFault),
            new JobResult.Unknown(ConfidenceLevel.PrintConfirmed,
                                  FailureReason.TimeoutAwaitingCompletion),
        ];

        // Confidence on a failure or an unknown is not decoration: it is how far up the
        // ladder the job got, which is what an operator needs to decide about a reprint.
        Assert.Equal(ConfidenceLevel.CutFaultFree, all[0].Confidence);
        Assert.Equal(ConfidenceLevel.PrinterHealthy, all[1].Confidence);
        Assert.Equal(ConfidenceLevel.PrintConfirmed, all[2].Confidence);
        Assert.Equal(FailureReason.None, all[0].Reason);
    }

    [Fact]
    public void AnExhaustiveSwitchCoversEveryCaseWithoutADefaultArm()
    {
        // Written the way an application writes it. If a fourth case were ever added, this
        // switch would start throwing at the discard arm rather than silently picking a
        // bucket -- which is the failure mode worth having.
        static string Describe(JobResult result) => result switch
        {
            JobResult.Done done => $"done:{done.Confidence}",
            JobResult.Failed failed => $"failed:{failed.Reason}",
            JobResult.Unknown unknown => $"unknown:{unknown.Reason}",
            _ => throw new InvalidOperationException("a fourth JobResult case exists"),
        };

        Assert.Equal("done:CutFaultFree",
                     Describe(new JobResult.Done(ConfidenceLevel.CutFaultFree,
                                                 FailureReason.None)));
        Assert.Equal("failed:PreflightPaperOut",
                     Describe(new JobResult.Failed(ConfidenceLevel.TransportAccepted,
                                                   FailureReason.PreflightPaperOut)));
        Assert.Equal("unknown:TimeoutAwaitingCompletion",
                     Describe(new JobResult.Unknown(ConfidenceLevel.PrintConfirmed,
                                                    FailureReason.TimeoutAwaitingCompletion)));
    }

    [Fact]
    public void AnUnrecognisedOutcomeDegradesToUnknownAndNeverToDone()
    {
        // The ABI is closed and asserted against the core at compile time, so this cannot
        // happen -- but if it ever did, the safe reading is "we do not know", never "it
        // worked". Reached through the internal converter, which is the only path a real
        // outcome takes.
        var native = new PdJobResult
        {
            Outcome = 99,
            Confidence = (int)ConfidenceLevel.TransportAccepted,
            Reason = (int)FailureReason.Unknown,
        };

        Assert.IsType<JobResult.Unknown>(JobResult.FromNative(in native));
    }
}
