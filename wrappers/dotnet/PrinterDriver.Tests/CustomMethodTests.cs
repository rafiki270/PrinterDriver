using System.Text;

namespace PrinterDriver.Tests;

/// <summary>
/// Custom method registration, end to end (docs/api.md §16).
/// </summary>
/// <remarks>
/// A made-up vendor idle scheme, <c>acme.x-idle</c>: the host sends <c>ESC 'x'</c> plus the
/// job's four-character verification token behind the payload, and an idle device echoes
/// <c>ESC 'y'</c> plus the same token. Registered as a completion method, that ack scheme
/// drives a grade-A completion path with no core release — and what this file proves is
/// that a .NET delegate can serve the fence and the matcher from a printer worker thread,
/// which is the whole of the wrapper's half of §16.
/// </remarks>
public class CustomMethodTests
{
    private static CompletionMethod AcmeIdle() => new()
    {
        Id = "acme.x-idle",
        Grade = ConfidenceGrade.AJobLevelConfirmation,
        Authority = CompletionAuthority.PhysicalPrinter,
        FenceBytes = token => [.. new byte[] { 0x1B, 0x78 }, .. Encoding.UTF8.GetBytes(token)],
        Matcher = bytes =>
        {
            for (var index = 0; index + 1 < bytes.Length; index++)
            {
                if (bytes[index] != 0x1B || bytes[index + 1] != 0x79)
                {
                    continue;
                }
                if (index + 6 > bytes.Length)
                {
                    return CompletionMatch.NeedMore;
                }
                return CompletionMatch.Matched(Encoding.UTF8.GetString(bytes, index + 2, 4));
            }
            return bytes.Length > 0 && bytes[^1] == 0x1B
                ? CompletionMatch.NeedMore
                : CompletionMatch.NotMine;
        },
    };

    [Fact]
    public void ARegisteredVendorCompletionMethodEarnsItsDeclaredGrade()
    {
        using var driver = TestDriver.Open();
        driver.RegisterCompletionMethod(AcmeIdle());

        // "vendor-idle" is a VendorIdle profile bound to the id registered above, behind a
        // device that echoes the ESC x fence.
        var printer = driver.AddScripted("vendor-idle", "dotnet-acme");
        Assert.Equal(CompletionMechanism.VendorIdle, printer.Completion);

        var job = printer.Print(Payload.FromText("ACME IDLE TICKET\nOrder 42"),
                                new JobOptions(Key: "acme-1"));
        var result = job.WaitForResult(5000);

        var done = Assert.IsType<JobResult.Done>(result);
        // The registered claim, attributed by id.
        Assert.Equal(ConfidenceGrade.AJobLevelConfirmation, done.Grade);
        Assert.Equal("A", done.Grade.Letter());
        Assert.Equal(CompletionAuthority.PhysicalPrinter, done.Authority);
        Assert.Equal("acme.x-idle", done.Method);
        // A vendor idle fence confirms print and the cut command, not a fault-free blade.
        Assert.Equal(ConfidenceLevel.CutProcessed, done.Confidence);

        // The custom fence promotes its per-job token to a resolvable verification
        // identifier, exactly like GS ( H: the ticket resolves back to this job.
        var token = job.PrintToken;
        Assert.NotNull(token);
        Assert.Equal(4, token!.Length);
        Assert.Same(job, driver.JobByToken(token));
        Assert.StartsWith(driver.InstanceNonce, token, StringComparison.Ordinal);
    }

    [Fact]
    public void ADeviceThatNeverIdlesLeavesTheJobUnknown()
    {
        using var driver = TestDriver.Open();
        driver.RegisterCompletionMethod(AcmeIdle());

        var printer = driver.AddScripted("vendor-idle-busy", "dotnet-acme-busy");
        var job = printer.Print(Payload.FromText("ACME IDLE TICKET"),
                                new JobOptions(Key: "acme-busy-1", TimeoutMs: 900));

        // Bytes went out and no ack came back. Not a success and not a failure.
        Assert.IsType<JobResult.Unknown>(job.WaitForResult(5000));
    }

    [Fact]
    public void ADuplicateOrIncompleteRegistrationIsRefused()
    {
        using var driver = TestDriver.Open();
        driver.RegisterCompletionMethod(AcmeIdle());
        Assert.Throws<PrinterDriverException>(() => driver.RegisterCompletionMethod(AcmeIdle()));
    }

    [Fact]
    public void AProbeStepMustNotBeAbleToPrint()
    {
        using var driver = TestDriver.Open();
        ProbeFinding Classify(byte[] response) => new(response.Length > 0, "acme-probe");

        // "Hi" is printable, and auto-detection must never cost a venue a roll of paper.
        Assert.Throws<PrinterDriverException>(() => driver.RegisterProbeStep(new ProbeStep
        {
            Id = "acme.printing-probe",
            RequestBytes = Encoding.ASCII.GetBytes("Hi"),
            Classify = Classify,
        }));

        // ESC ENQ: every byte below 0x20, so nothing it can do will mark paper.
        driver.RegisterProbeStep(new ProbeStep
        {
            Id = "acme.silent-probe",
            RequestBytes = [0x1B, 0x05],
            Classify = Classify,
        });
        Assert.Throws<PrinterDriverException>(() => driver.RegisterProbeStep(new ProbeStep
        {
            Id = "acme.silent-probe",
            RequestBytes = [0x1B, 0x05],
            Classify = Classify,
        }));
    }

    [Fact]
    public void TheOtherRegistrationPointsAcceptAndRefuseTheSameWay()
    {
        using var driver = TestDriver.Open();

        driver.RegisterBlockHandler(new BlockHandler
        {
            Kind = "acme.stamp",
            Render = (_, _) => BlockRendering.Rendered([0x1B, 0x64, 0x01]),
        });
        Assert.Throws<PrinterDriverException>(() => driver.RegisterBlockHandler(new BlockHandler
        {
            Kind = "acme.stamp",
            Render = (_, _) => BlockRendering.Degraded("already owned"),
        }));

        driver.RegisterFormatter(new TemplateFormatter
        {
            Name = "acme.upper",
            Format = (value, _, _) => value.ToUpperInvariant(),
        });
        Assert.Throws<PrinterDriverException>(() => driver.RegisterFormatter(new TemplateFormatter
        {
            Name = "acme.upper",
            Format = (value, _, _) => value,
        }));

        // No status pair: the method has no readable switch, so a kick reports
        // KickSentUnverified rather than a verified open.
        driver.RegisterDrawerKick(new DrawerKick
        {
            Id = "acme.kick",
            KickBytes = (channel, _) => [0x1B, 0x70, channel],
        });
        // The readable variant: both halves supplied, as pd.h requires them to be.
        driver.RegisterDrawerKick(new DrawerKick
        {
            Id = "acme.kick-sensed",
            KickBytes = (channel, _) => [0x1B, 0x70, channel],
            StatusRequest = () => [0x1D, 0x72, 0x02],
            StatusParse = bytes => bytes.Length == 0 ? null : (bytes[0] & 0x01) != 0,
        });
        Assert.Throws<PrinterDriverException>(() => driver.RegisterDrawerKick(new DrawerKick
        {
            Id = "acme.kick",
            KickBytes = (_, _) => [],
        }));
    }

    [Fact]
    public void AVerdictCarriesTheCoresOwnSpelling()
    {
        NativeFixture.Bind();
        Assert.Equal("Matched", CompletionMatch.Matched("AB12").AbiName);
        Assert.Equal("NotMine", CompletionMatch.NotMine.AbiName);
        Assert.Equal("NeedMore", CompletionMatch.NeedMore.AbiName);
        Assert.Equal("Fifo", DrainOrder.Fifo.AbiName());
        Assert.Equal("Queued", JobState.Queued.AbiName());
    }
}
