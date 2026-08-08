using System.Text;

namespace PrinterDriver.Tests;

/// <summary>
/// End-to-end behaviour through the ABI, against the scripted devices of
/// <c>capi/tests/pd_test_support.h</c>.
/// </summary>
/// <remarks>
/// These are the same scenarios <c>capi/tests/test_capi.c</c> covers at the C level, run
/// through the C# surface an application actually uses, so that a wrapper mistake — a
/// dropped event, a collapsed tri-state, a dedupe that returns a different object, a
/// callback delegate the GC took away — fails here rather than in a kitchen.
/// </remarks>
public sealed class PrintJobTests
{
    private static Payload Text(string text) => new Payload.Raw(Encoding.UTF8.GetBytes(text));

    [Fact]
    public async Task SubmittedJobReachesDoneOnAHealthyDevice()
    {
        using var driver = TestDriver.Open();
        var printer = driver.AddScripted("ok", "dotnet-e2e");

        var job = printer.Print(Text("END TO END VIA DOTNET"),
                                new JobOptions(Key: "dotnet-e2e-1"));
        var result = await job.GetResultAsync(TestTimeout.Token);

        var done = Assert.IsType<JobResult.Done>(result);
        // A GS ( H printer that also answers the post-cut cutter read: the claim is
        // allowed to reach the top of the ladder, and not one rung higher.
        Assert.Equal(ConfidenceLevel.CutFaultFree, done.Confidence);
        Assert.Equal(FailureReason.None, done.Reason);
        Assert.True(job.IsTerminal);
        Assert.Equal(JobState.DoneSoftware, job.CurrentState);
        Assert.Equal("dotnet-e2e-1", job.Key);
        Assert.Equal(1u, job.Attempt);
        Assert.Equal("dotnet-e2e", printer.Id);
        Assert.Equal(1, TestNative.pd_test_received_contains(printer.Handle,
                                                             "END TO END VIA DOTNET"));
        Assert.Equal(1u, (uint)TestNative.pd_test_cuts(printer.Handle));
    }

    [Fact]
    public async Task GsR1PrinterStopsAtTheConfidenceThatFenceSupports()
    {
        using var driver = TestDriver.Open();
        var printer = driver.AddScripted("gsr1");
        Assert.Equal(CompletionMechanism.GsR1, printer.Completion);

        var result = await printer.SendAsync(Text("ORDERED FENCE ONLY"),
                                             cancellationToken: TestTimeout.Token);

        var done = Assert.IsType<JobResult.Done>(result);
        // A second GS r 1 after the cut is an ordered fence, not a documented cutter
        // guarantee, so this caps one rung below CutFaultFree.
        Assert.Equal(ConfidenceLevel.CutProcessed, done.Confidence);
    }

    [Fact]
    public async Task SilentPrinterEndsUnknownAndNotFailed()
    {
        using var driver = TestDriver.Open();
        var printer = driver.AddScripted("silent");

        var job = printer.Print(Text("NO ANSWER EVER COMES"));
        var result = await job.GetResultAsync(TestTimeout.Token);

        // The bytes reached the device; the completion marker never came back. Calling
        // that a failure would authorise a reprint of a receipt that may already exist.
        var unknown = Assert.IsType<JobResult.Unknown>(result);
        Assert.Equal(FailureReason.TimeoutAwaitingCompletion, unknown.Reason);
        Assert.Equal(JobState.Unknown, job.CurrentState);
        Assert.Equal(1, TestNative.pd_test_received_contains(printer.Handle,
                                                             "NO ANSWER EVER COMES"));
    }

    [Fact]
    public async Task StrictPreflightRefusesBeforeASinglePayloadByte()
    {
        using var driver = TestDriver.Open();
        var printer = driver.AddScripted("paperout");

        var result = await printer.SendAsync(Text("MUST NEVER REACH PAPER"),
                                             cancellationToken: TestTimeout.Token);

        var failed = Assert.IsType<JobResult.Failed>(result);
        Assert.Equal(FailureReason.PreflightPaperOut, failed.Reason);
        Assert.Equal(0, TestNative.pd_test_received_contains(printer.Handle,
                                                             "MUST NEVER REACH PAPER"));
    }

    [Fact]
    public async Task SameKeyReturnsTheIdenticalJobAndPrintsOnce()
    {
        using var driver = TestDriver.Open();
        var printer = driver.AddScripted("ok");
        var options = new JobOptions(Key: "dotnet-dedupe-1");

        var first = printer.Print(Text("ONE COPY ONLY"), options);
        var second = printer.Print(Text("SHOULD NEVER PRINT"), options);

        // pd.h returns the same pd_job handle for a repeated key, and this wrapper interns
        // handles, so dedupe is visible as C# reference equality rather than as an equal
        // id that happens to match.
        Assert.Same(first, second);
        Assert.Equal(first.Id, second.Id);

        Assert.IsType<JobResult.Done>(await first.GetResultAsync(TestTimeout.Token));
        printer.Drain();

        Assert.Equal(1, TestNative.pd_test_received_contains(printer.Handle, "ONE COPY ONLY"));
        Assert.Equal(0, TestNative.pd_test_received_contains(printer.Handle,
                                                             "SHOULD NEVER PRINT"));
        Assert.Equal(1u, (uint)TestNative.pd_test_cuts(printer.Handle));

        // The driver's own key index resolves to the identical object too.
        Assert.Same(first, driver.FindJob("dotnet-dedupe-1"));
    }

    [Fact]
    public async Task ForceReprintIsTheDeliberateDuplicate()
    {
        using var driver = TestDriver.Open();
        var printer = driver.AddScripted("ok");
        var options = new JobOptions(Key: "dotnet-reprint-1");

        var first = printer.Print(Text("ORIGINAL TICKET"), options);
        Assert.IsType<JobResult.Done>(await first.GetResultAsync(TestTimeout.Token));

        var again = printer.ForceReprint("dotnet-reprint-1", options);
        Assert.NotSame(first, again);
        Assert.IsType<JobResult.Done>(await again.GetResultAsync(TestTimeout.Token));
        printer.Drain();

        Assert.Equal(2u, again.Attempt);
        Assert.Equal(2u, (uint)TestNative.pd_test_cuts(printer.Handle));
    }

    [Fact]
    public void ForceReprintOfAnUnknownKeyIsRefusedWithAReason()
    {
        using var driver = TestDriver.Open();
        var printer = driver.AddScripted("ok");

        var error = Assert.Throws<PrinterDriverException>(
            () => printer.ForceReprint("never-submitted"));
        Assert.Contains("never-submitted", error.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task EventsProgressInOrderAndEndOnATerminalEvent()
    {
        using var driver = TestDriver.Open();
        var printer = driver.AddScripted("ok");

        var job = printer.Print(Text("EVENT PROGRESSION"), new JobOptions(Key: "dotnet-events"));

        var events = new List<JobEvent>();
        await foreach (var jobEvent in job.EventsAsync(TestTimeout.Token))
        {
            events.Add(jobEvent);
        }

        Assert.NotEmpty(events);
        Assert.Equal(JobState.Queued, events[0].State);
        Assert.True(events[^1].IsTerminal);
        Assert.Equal(JobState.DoneSoftware, events[^1].State);
        Assert.Equal(ConfidenceLevel.CutFaultFree, events[^1].Confidence);

        // Exactly one terminal event, and it is the last one: an app that stops on the
        // first terminal event cannot miss anything, and cannot be handed a second.
        Assert.Single(events, e => e.IsTerminal);

        // Confidence never goes down, and every non-failure step carries no reason.
        for (var i = 1; i < events.Count; i++)
        {
            Assert.True(events[i].Confidence >= events[i - 1].Confidence,
                        $"confidence fell from {events[i - 1].Confidence} to {events[i].Confidence}");
            Assert.True(events[i].MonotonicMs >= events[i - 1].MonotonicMs);
        }
        Assert.All(events, e => Assert.Null(e.Reason));

        // The states the healthy path must pass through, in this order.
        var states = events.Select(e => e.State).ToList();
        Assert.Contains(JobState.PreflightOk, states);
        Assert.Contains(JobState.SendStarted, states);
        Assert.Contains(JobState.BytesSent, states);
        Assert.True(states.IndexOf(JobState.SendStarted) < states.IndexOf(JobState.BytesSent));
    }

    [Fact]
    public async Task JobEventsReplayForALateSubscriber()
    {
        using var driver = TestDriver.Open();
        var printer = driver.AddScripted("ok");

        var job = printer.Print(Text("LATE SUBSCRIBER"), new JobOptions(Key: "dotnet-late"));
        Assert.IsType<JobResult.Done>(await job.GetResultAsync(TestTimeout.Token));

        // pd_subscribe_job replays the whole recorded history synchronously before it
        // returns, so subscribing after the job is already terminal still sees all of it.
        var events = new List<JobEvent>();
        await foreach (var jobEvent in job.EventsAsync(TestTimeout.Token))
        {
            events.Add(jobEvent);
        }

        Assert.Equal(JobState.Queued, events[0].State);
        Assert.Equal(JobState.DoneSoftware, events[^1].State);
    }

    [Fact]
    public async Task DeviceEventsReachASubscriber()
    {
        using var driver = TestDriver.Open();
        var printer = driver.AddScripted("ok");

        var collected = new List<DeviceEvent>();
        var handlerSaw = 0;
        void OnEvent(object? sender, DeviceEvent e) => Interlocked.Increment(ref handlerSaw);
        printer.DeviceEventReceived += OnEvent;

        // The device stream has no natural end -- a printer is never "done" -- so the
        // reader is stopped with its own token rather than by disposing the enumerator
        // out from under the task that is enumerating it.
        using var stop = new CancellationTokenSource(TimeSpan.FromSeconds(30));
        var stream = printer.EventsAsync(stop.Token);
        var pump = Task.Run(async () =>
        {
            try
            {
                await foreach (var deviceEvent in stream)
                {
                    lock (collected)
                    {
                        collected.Add(deviceEvent);
                    }
                }
            }
            catch (OperationCanceledException)
            {
                // Expected: this is how the enumeration is ended.
            }
        }, CancellationToken.None);

        Assert.IsType<JobResult.Done>(
            await printer.SendAsync(Text("DEVICE EVENTS"), cancellationToken: TestTimeout.Token));
        printer.Drain();
        // The stream has no natural end; give the worker thread a moment to hand its last
        // event over before looking.
        await Task.Delay(250);

        List<DeviceEvent> seen;
        lock (collected)
        {
            seen = [.. collected];
        }
        printer.DeviceEventReceived -= OnEvent;

        Assert.NotEmpty(seen);
        Assert.All(seen, e => Assert.True(Enum.IsDefined(e), $"undeclared device event {(int)e}"));
        // A healthy device reports itself healthy; that is what makes polling unnecessary.
        Assert.Contains(DeviceEvent.Online, seen);
        Assert.Equal(seen.Count, handlerSaw);

        await stop.CancelAsync();
        await pump.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public void DeviceStatusIsUnobservedBeforeAnythingHasBeenHeard()
    {
        using var driver = TestDriver.Open();
        var printer = driver.AddScripted("ok");

        var status = printer.Status;

        // Never a live query, and never optimistic: a snapshot that has not heard from the
        // device says so rather than reporting healthy.
        Assert.False(status.Observed);
        Assert.Null(status.Online);
        Assert.Null(status.PaperOut);
    }

    [Fact]
    public void ProfileIdsComeFromTheAbiRatherThanAHardcodedList()
    {
        NativeFixture.Bind();
        var ids = PrinterDriver.ProfileIds;

        Assert.NotEmpty(ids);
        Assert.Contains("generic", ids);
        Assert.All(ids, id => Assert.False(string.IsNullOrWhiteSpace(id)));
    }

    [Fact]
    public void AnUnreachablePrinterFailsTheJobRatherThanTheDriver()
    {
        using var driver = TestDriver.Open();
        var printer = driver.AddScripted("refuse");

        var result = printer.Print(Text("NOWHERE TO GO")).WaitForResult(10_000);

        var failed = Assert.IsType<JobResult.Failed>(result);
        Assert.Equal(FailureReason.TransportUnreachable, failed.Reason);
        Assert.Equal(ConfidenceLevel.TransportAccepted, failed.Confidence);
    }
}

/// <summary>A ceiling on every wait, so a hung ABI call fails the run instead of hanging it.</summary>
internal static class TestTimeout
{
    internal static CancellationToken Token => new CancellationTokenSource(
        TimeSpan.FromSeconds(30)).Token;
}
