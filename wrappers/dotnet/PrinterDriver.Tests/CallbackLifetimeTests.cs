using System.Text;

namespace PrinterDriver.Tests;

/// <summary>
/// The one failure mode a P/Invoke wrapper of a callback API almost always ships with:
/// a delegate the GC collected while the unmanaged side still held a pointer to it.
/// </summary>
/// <remarks>
/// <para>
/// pd.h has no <c>pd_unsubscribe_*</c>, so between <c>pd_subscribe_job</c> and
/// <c>pd_destroy</c> the core may call back at any moment from a printer worker thread. A
/// delegate referenced only from unmanaged memory is unreachable as far as the GC is
/// concerned. Without the <see cref="System.Runtime.InteropServices.GCHandle"/> roots in
/// <c>CallbackRoots</c>, a collection between subscribing and the next event turns the
/// function pointer into a dangling stub, and the process dies inside a native thread with
/// a stack trace naming nothing in this repository.
/// </para>
/// <para>
/// These tests force that collection, deliberately and repeatedly, at the exact points
/// where an unrooted delegate would be reclaimed.
/// </para>
/// </remarks>
public sealed class CallbackLifetimeTests
{
    private static Payload Text(string text) => new Payload.Raw(Encoding.UTF8.GetBytes(text));

    private static void CollectHard()
    {
        for (var pass = 0; pass < 3; pass++)
        {
            GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, blocking: true,
                       compacting: true);
            GC.WaitForPendingFinalizers();
        }
    }

    [Fact]
    public async Task DeviceSubscriptionSurvivesAFullCollectionBeforeAnyEventArrives()
    {
        using var driver = TestDriver.Open();
        var printer = driver.AddScripted("ok");

        using var stop = new CancellationTokenSource(TimeSpan.FromSeconds(30));
        var collected = new List<DeviceEvent>();
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
            }
        }, CancellationToken.None);

        // Subscribed, nothing delivered yet, and now every managed reference the caller
        // could have kept is gone. If the delegate were not rooted, the print below would
        // call a collected stub from a worker thread.
        CollectHard();

        Assert.IsType<JobResult.Done>(
            await printer.SendAsync(Text("AFTER A COLLECTION"),
                                    cancellationToken: TestTimeout.Token));
        printer.Drain();
        await Task.Delay(250);

        List<DeviceEvent> seen;
        lock (collected)
        {
            seen = [.. collected];
        }
        Assert.NotEmpty(seen);

        await stop.CancelAsync();
        await pump.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task JobSubscriptionSurvivesCollectionsThroughoutTheJob()
    {
        using var driver = TestDriver.Open();
        var printer = driver.AddScripted("silent");  // slow: the fence never answers

        var job = printer.Print(Text("COLLECT WHILE I RUN"),
                                new JobOptions(Key: "dotnet-gc-1"));
        var stream = job.EventsAsync(TestTimeout.Token);

        var events = new List<JobEvent>();
        var reader = Task.Run(async () =>
        {
            await foreach (var jobEvent in stream)
            {
                lock (events)
                {
                    events.Add(jobEvent);
                }
            }
        }, CancellationToken.None);

        // Collect repeatedly while the job is still in flight and the worker thread is
        // still calling the trampoline.
        for (var i = 0; i < 5; i++)
        {
            CollectHard();
            await Task.Delay(25);
        }

        await reader.WaitAsync(TimeSpan.FromSeconds(30));

        List<JobEvent> seen;
        lock (events)
        {
            seen = [.. events];
        }
        Assert.NotEmpty(seen);
        Assert.True(seen[^1].IsTerminal);
        // The silent device never answers the completion marker, so the only honest end
        // is Unknown -- reached through a callback that was alive the whole way.
        Assert.Equal(JobState.Unknown, seen[^1].State);
    }

    [Fact]
    public async Task DisposingTheDriverCompletesEveryOpenEventStream()
    {
        var driver = TestDriver.Open();
        var printer = driver.AddScripted("ok");
        var job = printer.Print(Text("STREAM THEN DISPOSE"));
        var stream = job.EventsAsync(CancellationToken.None);

        driver.Dispose();

        // pd_destroy has returned, so the roots are gone and nothing can call back. A
        // stream left open must end rather than hang forever on a driver that no longer
        // exists.
        var drain = Task.Run(async () =>
        {
            await foreach (var _ in stream)
            {
            }
        });
        await drain.WaitAsync(TimeSpan.FromSeconds(10));
    }

    [Fact]
    public void DisposingTwiceIsSafe()
    {
        var driver = TestDriver.Open();
        driver.AddScripted("ok");

        driver.Dispose();
        driver.Dispose();

        // And nothing works afterwards, loudly rather than by crashing in native code.
        Assert.Throws<ObjectDisposedException>(
            () => driver.AddPrinterTcp(new TcpPrinterConfig("192.0.2.1")));
    }
}
