using System.Text;

namespace PrinterDriver.Tests;

/// <summary>
/// M13b. The print-queue addon through the ABI (docs/sdk-spec.md section 12).
/// </summary>
/// <remarks>
/// One happy path, driven against the real engine over the real <c>pd_queue_*</c> surface,
/// so what is under test is the binding plus the addon rather than a rehearsal of the
/// binding's own assumptions.
/// </remarks>
public sealed class PrintQueueTests
{
    private static Payload Text(string text) => new Payload.Raw(Encoding.UTF8.GetBytes(text));

    [Fact]
    public async Task EnqueuedJobDrainsThroughTheSameEngineAndEarnsTheSameGrade()
    {
        using var driver = TestDriver.Open();
        var printer = driver.AddScripted("ok", "dotnet-queue");
        using var queue = new PrintQueue(driver);

        Assert.False(queue.IsPaused(printer.Id));
        Assert.False(queue.IsBlocked(printer.Id));

        var job = queue.Enqueue(printer, Text("QUEUED TICKET"),
                                new QueueOptions { Key = "dotnet-queued-1" });
        Assert.Equal("dotnet-queued-1", job.Key);

        var result = await job.GetResultAsync(TestTimeout.Token);
        // Rule 3 of section 12, observable: a queued job goes down the identical engine
        // path a direct print takes, so it earns the identical claim from the identical
        // fence.
        var done = Assert.IsType<JobResult.Done>(result);
        Assert.Equal(ConfidenceGrade.AJobLevelConfirmation, done.Grade);
        Assert.Equal(CompletionAuthority.PhysicalPrinter, done.Authority);
        Assert.Equal("GS(H) fn48", done.Method);

        // Rule 2: the key is claimed in the driver's own index at enqueue time, so a
        // direct print of the same key finds the queued job instead of producing a second
        // receipt.
        var deduped = printer.Print(Text("DUPLICATE"), new JobOptions(Key: "dotnet-queued-1"));
        Assert.Equal(job.Id, deduped.Id);

        Assert.Equal(0ul, queue.Pending());
        Assert.Equal(0ul, queue.Pending(printer.Id));
        Assert.Equal(0ul, queue.ExpiredCount);
        Assert.Equal(0ul, queue.OverflowCount);
        queue.Tick();

        // Operator hold is independent of anything the device is reporting.
        queue.Pause(printer.Id);
        Assert.True(queue.IsPaused(printer.Id));
        queue.Resume(printer.Id);
        Assert.False(queue.IsPaused(printer.Id));
    }
}
