using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text;

namespace PrinterDriver.Tests;

/// <summary>
/// The platform-owned link, driven through the managed binding rather than around it.
/// </summary>
/// <remarks>
/// <para>
/// docs/compatibility-brief.md §25. What has to be proved here is not that bytes move. It
/// is that a printer reached over a link the core knows nothing about still gets the whole
/// ordered fence and still reports exactly the claim a TCP printer would — because the
/// moment a transport could weaken the completion story, every wrapper would become part of
/// it, and this one is written in C# by whoever owns the Bluetooth socket.
/// </para>
/// <para>
/// <c>capi/tests/test_capi.c</c> covers the same ground with a vtable written in C over a
/// scripted device supplied by <c>pd_test_support</c>. This suite deliberately does not
/// reuse that device: the scripted printer below is implemented in C#, so the bytes go out
/// through the managed <see cref="IPrinterTransport"/> delegates and come back in through
/// <see cref="CustomTransportPrinter.FeedBytes(ReadOnlySpan{byte})"/>. A marshalling
/// mistake in either direction fails here rather than being hidden behind a C double.
/// </para>
/// </remarks>
public sealed class CustomTransportTests
{
    private static Payload Text(string text) => new Payload.Raw(Encoding.UTF8.GetBytes(text));

    // Every test here declares the driver before the transport, so `using` tears the
    // transport's RX thread down first: pd_destroy frees every printer handle, and a
    // delivery still in flight when it does would be reading freed memory. That ordering is
    // the same one an application has to get right, and it is stated once here rather than
    // repeated in each test.

    [Fact]
    public async Task ACustomTransportEarnsTheSameClaimATcpPrinterWould()
    {
        using var driver = TestDriver.Open();
        using var transport = new ScriptedPrinterTransport();
        var link = driver.AddPrinterCustom(transport, new CustomPrinterConfig(
            Description: "bt-spp:00:11:22:33:44:55", ProfileId: "xp-s260m", WidthDots: 576));
        transport.Bind(link);

        // The id derives from the vtable's description, so two Bluetooth printers are
        // distinguishable without the caller inventing names for them.
        Assert.Contains("bt-spp", link.Printer.Id, StringComparison.Ordinal);
        Assert.Equal(576u, link.Printer.WidthDots);
        Assert.Equal(CompletionMechanism.GsParenH, link.Printer.Completion);

        var result = await link.Printer.SendAsync(Text("BLUETOOTH TICKET"),
                                                  new JobOptions(Key: "dotnet-bt-1"),
                                                  TestTimeout.Token);

        var done = Assert.IsType<JobResult.Done>(result);
        Assert.Equal(ConfidenceLevel.CutFaultFree, done.Confidence);
        Assert.Equal(ConfidenceGrade.AJobLevelConfirmation, done.Grade);
        Assert.Equal(CompletionAuthority.PhysicalPrinter, done.Authority);
        Assert.Equal("GS(H) fn48", done.Method);
        // Not weaker for being a caller-owned link, and not stronger either: A is the top
        // of what this core can produce, because the A+ mechanism is an ePOS job and no
        // ePOS transport exists here.
        Assert.NotEqual(ConfidenceGrade.APlusDurableQueryableJob, done.Grade);

        link.Printer.Drain();
        Assert.True(transport.Connects >= 1, "the core never opened the link");
        Assert.True(transport.BytesWritten > 0);
        Assert.Equal(1, transport.Cuts);
        Assert.True(transport.ReceivedContains("BLUETOOTH TICKET"));

        // The fence answers came back on the transport's own RX thread, not on the worker
        // thread that called Write. pd.h requires that, and a scripted device that
        // answered inline would prove nothing about the contract this binding has to keep.
        Assert.NotEqual(0, transport.WriteThreadId);
        Assert.NotEqual(0, transport.FeedThreadId);
        Assert.NotEqual(transport.WriteThreadId, transport.FeedThreadId);

        transport.StopReader();
        driver.Dispose();
        // Disposing the driver closes the link. On the Epson portables that matters twice
        // over: the radio stays paired and the one allowed connection slot is freed.
        Assert.True(transport.Closes >= 1);
    }

    [Theory]
    [InlineData("zebra_zq600_plus")]
    [InlineData("brother_rj4000")]
    public async Task AZplOrBrotherProfileIsRefusedBeforeAByteReachesTheLink(string profileId)
    {
        // docs/compatibility-brief.md §16, §17. ZPL, CPCL and Brother raster are not
        // ESC/POS at any level. The refusal has to happen before the link is opened -- a
        // refusal that still wasted a roll would be worse than the bug it replaced.
        using var driver = TestDriver.Open();
        using var transport = new ScriptedPrinterTransport();
        var link = driver.AddPrinterCustom(
            transport, new CustomPrinterConfig(Description: profileId, ProfileId: profileId));
        transport.Bind(link);

        Assert.NotEqual(CommandLanguage.EscPos, link.Printer.Language);

        var result = await link.Printer.SendAsync(Text("LABEL"),
                                                  cancellationToken: TestTimeout.Token);

        var failed = Assert.IsType<JobResult.Failed>(result);
        Assert.Equal(FailureReason.Unsupported, failed.Reason);
        Assert.Equal(ConfidenceGrade.ETransportOnly, failed.Grade);
        Assert.Equal(CompletionAuthority.TransportOnly, failed.Authority);
        Assert.Equal("none", failed.Method);

        // Zero bytes, zero connections, zero cuts, zero paper.
        Assert.Equal(0L, transport.BytesWritten);
        Assert.Equal(0, transport.Connects);
        Assert.Equal(0, transport.Cuts);
    }

    [Fact]
    public void AnUnknownProfileIsRefusedRatherThanSilentlyDowngraded()
    {
        // A caller that asked for a TM-T88VI and was handed the unknown-device profile
        // would be told a weaker completion story than it asked for, with nothing in the
        // result to explain why.
        using var driver = TestDriver.Open();
        using var transport = new ScriptedPrinterTransport();

        var error = Assert.Throws<PrinterDriverException>(
            () => driver.AddPrinterCustom(transport,
                                          new CustomPrinterConfig(ProfileId: "epson_tm_t99")));
        Assert.Contains("epson_tm_t99", error.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void FeedingBytesFromInsideATransportCallbackIsRefusedRatherThanDeadlocking()
    {
        using var driver = TestDriver.Open();
        var transport = new ReentrantTransport();
        var link = driver.AddPrinterCustom(transport,
                                           new CustomPrinterConfig(ProfileId: "xp-s260m"));
        transport.Bind(link);

        var result = link.Printer.Print(Text("NEVER SENT")).WaitForResult(20_000);

        // pd.h: feed_bytes must not be called from inside connect/write/close, because
        // those run on the thread that would have to service the delivery. The binding says
        // so, with the fix in the message, instead of parking a worker thread on itself and
        // surfacing as a job that never finishes.
        var refusal = Assert.IsType<InvalidOperationException>(transport.Refusal);
        Assert.Contains("RX thread", refusal.Message, StringComparison.Ordinal);
        var failed = Assert.IsType<JobResult.Failed>(result);
        Assert.Equal(FailureReason.TransportUnreachable, failed.Reason);
    }

    [Fact]
    public void BytesFedWithNoOneListeningAreDroppedRatherThanQueued()
    {
        using var driver = TestDriver.Open();
        using var transport = new ScriptedPrinterTransport();
        var link = driver.AddPrinterCustom(transport,
                                           new CustomPrinterConfig(ProfileId: "xp-s260m"));

        // Nothing has connected yet, so there is no response parser to receive this. That
        // is information rather than an error -- the same thing that happens to bytes
        // arriving on a socket nobody is reading -- and it must not be reported as success.
        Assert.False(link.FeedBytes(new byte[] { 0x16 }));
        Assert.False(link.FeedBytes(ReadOnlySpan<byte>.Empty));
        Assert.False(link.LinkDropped("never connected"));
    }

    [Fact]
    public async Task ADroppedLinkEndsAJobWaitingOnTheFenceInsteadOfLettingItTimeOut()
    {
        // The peer went out of range mid-receipt. Without the report the job would sit out
        // the profile's 15-second completion timeout before admitting it does not know.
        using var driver = TestDriver.Open();
        using var transport = new ScriptedPrinterTransport { AnswerProcessId = false };
        var link = driver.AddPrinterCustom(transport,
                                           new CustomPrinterConfig(ProfileId: "xp-s260m"));
        transport.Bind(link);

        var job = link.Printer.Print(Text("DROPPED MID FENCE"),
                                     new JobOptions(Key: "dotnet-bt-drop"));
        var pending = job.GetResultAsync(TestTimeout.Token);

        var deadline = Stopwatch.StartNew();
        while (!transport.ReceivedContains("DROPPED MID FENCE") &&
               deadline.Elapsed < TimeSpan.FromSeconds(10))
        {
            await Task.Delay(10);
        }
        Assert.True(transport.ReceivedContains("DROPPED MID FENCE"),
                    "the payload never reached the link, so there was no fence to drop");

        var dropped = Stopwatch.StartNew();
        Assert.True(link.LinkDropped("peer went out of range"));
        var result = await pending;
        dropped.Stop();

        // The bytes went out and the fence never answered, so Done is not available and
        // Failed would authorise a reprint of a receipt that may already exist.
        Assert.IsType<JobResult.Unknown>(result);
        Assert.True(dropped.Elapsed < TimeSpan.FromSeconds(10),
                    $"the drop was ignored and the job sat out its timeout ({dropped.Elapsed})");
    }
}

/// <summary>
/// A scripted printer on the far side of a caller-owned link, implemented in C#.
/// </summary>
/// <remarks>
/// <para>
/// The same shape as <c>pd_test_link</c> in <c>capi/tests/pd_test_support.cpp</c>, and for
/// the same reason: responses are queued by <see cref="Write"/> and delivered by a separate
/// reader thread, because pd.h forbids feeding bytes from inside a transport callback and
/// every real stack behaves this way anyway — a CoreBluetooth delegate queue, an Android
/// reader thread, a BlueZ recv loop.
/// </para>
/// <para>
/// The command scanner mirrors <c>core/tests/fake_printer.hpp</c>, whose one load-bearing
/// property is that the stream is FIFO: a queued answer (the GS ( H echo, the GS r reply)
/// is produced when the scanner reaches that command, after everything printed ahead of it.
/// That ordering is the whole basis of the completion fence, so a device that answered out
/// of order would let a wrapper bug pass.
/// </para>
/// </remarks>
internal sealed class ScriptedPrinterTransport : IPrinterTransport, IDisposable
{
    // The healthy DLE EOT answers of core/tests/fake_printer.hpp: online with the drawer
    // pin high, cover closed, no cutter fault, paper present.
    private const byte PrinterStatus = 0x16;
    private const byte OfflineCause = 0x12;
    private const byte ErrorCause = 0x12;
    private const byte PaperSensor = 0x12;
    private static readonly byte[] AsbFrame = [0x10, 0x10, 0x10, 0x10];

    private readonly BlockingCollection<byte[]> _outbound = new();
    private readonly List<byte> _received = [];
    private readonly List<byte> _pending = [];
    private readonly object _gate = new();
    private readonly Thread _reader;

    private CustomTransportPrinter? _printer;
    private int _connects;
    private int _closes;
    private int _cuts;
    private long _bytesWritten;
    private int _writeThreadId;
    private int _feedThreadId;

    internal ScriptedPrinterTransport()
    {
        _reader = new Thread(ReaderLoop) { IsBackground = true, Name = "scripted-printer-rx" };
        _reader.Start();
    }

    /// <summary>False makes the device stop echoing the GS ( H process id, as a link that
    /// swallows the backchannel would.</summary>
    internal bool AnswerProcessId { get; init; } = true;

    internal int Connects => Volatile.Read(ref _connects);
    internal int Closes => Volatile.Read(ref _closes);
    internal int Cuts => Volatile.Read(ref _cuts);
    internal long BytesWritten => Interlocked.Read(ref _bytesWritten);
    internal int WriteThreadId => Volatile.Read(ref _writeThreadId);
    internal int FeedThreadId => Volatile.Read(ref _feedThreadId);

    /// <summary>Tells the reader thread which printer to feed. Call after attaching.</summary>
    internal void Bind(CustomTransportPrinter printer) => Volatile.Write(ref _printer, printer);

    internal bool ReceivedContains(string needle)
    {
        var wanted = Encoding.UTF8.GetBytes(needle);
        lock (_gate)
        {
            for (var start = 0; start + wanted.Length <= _received.Count; start++)
            {
                var match = true;
                for (var i = 0; match && i < wanted.Length; i++)
                {
                    match = _received[start + i] == wanted[i];
                }
                if (match)
                {
                    return true;
                }
            }
        }
        return false;
    }

    /// <summary>Stops delivering and waits for the reader thread to finish.</summary>
    internal void StopReader()
    {
        if (!_outbound.IsAddingCompleted)
        {
            _outbound.CompleteAdding();
        }
        _reader.Join(TimeSpan.FromSeconds(5));
    }

    public bool Connect()
    {
        Interlocked.Increment(ref _connects);
        return true;
    }

    public long Write(ReadOnlySpan<byte> data)
    {
        Volatile.Write(ref _writeThreadId, Environment.CurrentManagedThreadId);
        var copy = data.ToArray();
        byte[] response;
        lock (_gate)
        {
            Interlocked.Add(ref _bytesWritten, copy.Length);
            _received.AddRange(copy);
            _pending.AddRange(copy);
            response = Scan();
        }
        if (response.Length > 0 && !_outbound.IsAddingCompleted)
        {
            _outbound.Add(response);
        }
        return copy.Length;
    }

    public void Close() => Interlocked.Increment(ref _closes);

    public void Dispose()
    {
        StopReader();
        _outbound.Dispose();
    }

    private void ReaderLoop()
    {
        try
        {
            foreach (var chunk in _outbound.GetConsumingEnumerable())
            {
                Volatile.Write(ref _feedThreadId, Environment.CurrentManagedThreadId);
                try
                {
                    Volatile.Read(ref _printer)?.FeedBytes(chunk);
                }
                catch (ObjectDisposedException)
                {
                    // The driver went away while this chunk was in the queue. A real RX
                    // loop ends the same way, which is why FeedBytes reports it rather than
                    // reaching into a freed handle.
                    return;
                }
            }
        }
        catch (InvalidOperationException)
        {
            // GetConsumingEnumerable races CompleteAdding on shutdown.
        }
    }

    /// <summary>
    /// Consumes whole commands from the head of the buffer and returns what the device
    /// would send back, leaving any partial trailing command for the next write.
    /// </summary>
    private byte[] Scan()
    {
        var output = new List<byte>();
        var offset = 0;
        while (offset < _pending.Count)
        {
            var first = _pending[offset];

            if (first == 0x10 && offset + 2 < _pending.Count && _pending[offset + 1] == 0x04)
            {
                // DLE EOT n: real-time, answered the moment it is scanned. This is what
                // preflight asks and what makes a cover-open device refuse before printing.
                var answer = _pending[offset + 2] switch
                {
                    1 => PrinterStatus,
                    2 => OfflineCause,
                    3 => ErrorCause,
                    4 => PaperSensor,
                    _ => (byte)0,
                };
                if (answer != 0)
                {
                    output.Add(answer);
                }
                offset += 3;
                continue;
            }
            if (first == 0x10 && offset + 2 >= _pending.Count)
            {
                break;
            }

            if (first == 0x1D && offset + 1 < _pending.Count)
            {
                var second = _pending[offset + 1];
                if (second == 0x28)
                {
                    // GS ( X pL pH ... -- the only one this core sends is GS ( H fn48, the
                    // process-id marker that carries the four-character correlation token.
                    if (offset + 4 >= _pending.Count)
                    {
                        break;
                    }
                    var length = _pending[offset + 3] | (_pending[offset + 4] << 8);
                    var total = 5 + length;
                    if (offset + total > _pending.Count)
                    {
                        break;
                    }
                    if (_pending[offset + 2] == 0x48 && length == 6 &&
                        _pending[offset + 5] == 0x30 && AnswerProcessId)
                    {
                        // Echoed back verbatim inside the 0x37 0x22 <token> NUL frame the
                        // parser expects; a token invented here would be attributed to no
                        // job and read as a foreign writer.
                        output.Add(0x37);
                        output.Add(0x22);
                        output.AddRange(_pending.GetRange(offset + 7, 4));
                        output.Add(0x00);
                    }
                    offset += total;
                    continue;
                }
                if (second == 0x72 && offset + 2 < _pending.Count)
                {
                    output.Add(0x00);  // GS r n: queued status, no fault
                    offset += 3;
                    continue;
                }
                if (second == 0x49)
                {
                    // GS I n: identification. Left unanswered, as a device that does not
                    // implement it would; the driver must not need it to complete a job.
                    if (offset + 2 >= _pending.Count)
                    {
                        break;
                    }
                    offset += 3;
                    continue;
                }
                if (second == 0x61)
                {
                    if (offset + 2 >= _pending.Count)
                    {
                        break;
                    }
                    if (_pending[offset + 2] != 0x00)
                    {
                        output.AddRange(AsbFrame);  // GS a n: automatic status back enabled
                    }
                    offset += 3;
                    continue;
                }
                if (second == 0x76 && offset + 2 < _pending.Count && _pending[offset + 2] == 0x30)
                {
                    if (offset + 7 >= _pending.Count)
                    {
                        break;
                    }
                    var bytesPerRow = _pending[offset + 4] | (_pending[offset + 5] << 8);
                    var rows = _pending[offset + 6] | (_pending[offset + 7] << 8);
                    var total = 8 + (bytesPerRow * rows);
                    if (offset + total > _pending.Count)
                    {
                        break;
                    }
                    offset += total;  // GS v 0: raster block
                    continue;
                }
                if (second == 0x56)
                {
                    if (offset + 2 >= _pending.Count)
                    {
                        break;
                    }
                    var mode = _pending[offset + 2];
                    var total = mode is 65 or 66 ? 4 : 3;
                    if (offset + total > _pending.Count)
                    {
                        break;
                    }
                    Interlocked.Increment(ref _cuts);  // GS V: cut
                    offset += total;
                    continue;
                }
            }

            var fixedLength = FixedCommandLength(offset);
            if (fixedLength != 0)
            {
                if (offset + fixedLength > _pending.Count)
                {
                    break;
                }
                offset += fixedLength;
                continue;
            }
            if ((first == 0x1B || first == 0x1D) && offset + 1 >= _pending.Count)
            {
                break;  // Escape byte whose selector is still in flight.
            }

            offset++;  // Print data: paper, as far as this device is concerned.
        }
        _pending.RemoveRange(0, offset);
        return [.. output];
    }

    /// <summary>Length of the fixed-size command at <paramref name="offset"/>, or 0.</summary>
    private int FixedCommandLength(int offset)
    {
        var first = _pending[offset];
        var second = offset + 1 < _pending.Count ? _pending[offset + 1] : (byte)0;
        if (first == 0x1B)
        {
            return second switch
            {
                0x40 => 2,                                          // ESC @
                0x74 or 0x61 or 0x45 or 0x2D or 0x64 or 0x4A => 3,   // ESC t/a/E/-/d/J
                0x70 => 5,                                          // ESC p
                _ => 0,
            };
        }
        if (first == 0x1D)
        {
            return second switch { 0x21 or 0x61 => 3, _ => 0 };     // GS ! / GS a
        }
        return 0;
    }
}

/// <summary>
/// A transport that breaks the thread contract on purpose: it feeds bytes back from inside
/// <see cref="Connect"/>, which is the mistake pd.h spells out and which would otherwise be
/// a worker thread waiting on itself.
/// </summary>
internal sealed class ReentrantTransport : IPrinterTransport
{
    private CustomTransportPrinter? _printer;

    internal Exception? Refusal { get; private set; }

    internal void Bind(CustomTransportPrinter printer) => _printer = printer;

    public bool Connect()
    {
        try
        {
            _printer?.FeedBytes(new byte[] { 0x16 });
        }
        catch (Exception exception)
        {
            Refusal = exception;
        }
        return false;
    }

    public long Write(ReadOnlySpan<byte> data) => -1;

    public void Close()
    {
    }
}
