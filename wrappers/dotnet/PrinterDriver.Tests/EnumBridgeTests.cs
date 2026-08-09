using System.Runtime.InteropServices;

namespace PrinterDriver.Tests;

/// <summary>
/// The mirroring guarantee, checked one layer further out than the C suite checks it.
/// </summary>
/// <remarks>
/// <para>
/// <c>capi/tests/test_capi.c</c> proves pd.h and the C++ core agree. This proves the C#
/// enums agree with both — member count, member value and, where the core has a spelling,
/// member name — by asking the enum bridge of <c>capi/tests/pd_test_support.h</c> what the
/// core says rather than by trusting a comment. A member added to the core therefore fails
/// here instead of reaching an application as a silent hole.
/// </para>
/// <para>
/// Three independent statements of every count have to agree: what C# declares, what
/// <c>PD_*_COUNT</c> in pd.h says (transcribed below), and what the core's own
/// <c>kAll*</c> arrays report through the bridge. pd_capi.cpp's static_asserts close the
/// loop between the second and the third at C++ compile time; this closes it to C#.
/// </para>
/// </remarks>
public sealed class EnumBridgeTests
{
    /// <summary>One mirrored enum: what C# says, and how to ask the native side the same.</summary>
    private sealed record Mirror(
        BridgedEnum Bridge,
        /// The PD_*_COUNT constant from capi/include/printerdriver/pd.h, transcribed.
        int HeaderCount,
        Type EnumType,
        /// False where the C# spelling cannot be compared with the core's character for
        /// character: either the core has no to_string, or -- ConfidenceGrade alone -- it
        /// separates the grade letter with an underscore ("A_JobLevelConfirmation"), which
        /// is not a C# identifier. ConfidenceGrade's spelling is checked in
        /// TheGradeLadderGainedAPlusAtItsTop instead, with the underscores taken out.
        bool HasComparableCoreNames);

    private static readonly Mirror[] Mirrors =
    [
        new(BridgedEnum.JobState, 11, typeof(JobState), true),
        new(BridgedEnum.Confidence, 6, typeof(ConfidenceLevel), true),
        new(BridgedEnum.DeviceEvent, 13, typeof(DeviceEvent), true),
        new(BridgedEnum.FailureReason, 11, typeof(FailureReason), true),
        new(BridgedEnum.JobOutcome, 3, typeof(JobOutcome), true),
        new(BridgedEnum.Cut, 4, typeof(CutSetting), false),
        new(BridgedEnum.Preflight, 2, typeof(PreflightMode), false),
        new(BridgedEnum.PayloadKind, 3, typeof(PayloadKind), true),
        new(BridgedEnum.Completion, 6, typeof(CompletionMechanism), true),
        new(BridgedEnum.CutVariant, 3, typeof(CutVariant), true),
        new(BridgedEnum.Alignment, 3, typeof(Alignment), false),
        new(BridgedEnum.CodePage, 5, typeof(CodePage), false),
        new(BridgedEnum.Binarization, 2, typeof(Binarization), false),
        new(BridgedEnum.ConfidenceGrade, 6, typeof(ConfidenceGrade), false),
        new(BridgedEnum.CompletionAuthority, 5, typeof(CompletionAuthority), true),
    ];

    public EnumBridgeTests() => NativeFixture.Bind();

    [Fact]
    public void EveryBridgedEnumIsMirrored()
    {
        // PD_TEST_ENUM_TOTAL is 15: if the bridge grows an entry and this table does not,
        // the new enum would simply never be checked.
        Assert.Equal(15, Mirrors.Length);
        Assert.Equal(Mirrors.Length, Mirrors.Select(m => m.Bridge).Distinct().Count());
    }

    [Fact]
    public void MemberCountsAgreeAcrossCSharpTheHeaderAndTheCore()
    {
        foreach (var mirror in Mirrors)
        {
            var label = TestNative.ReadUtf8(TestNative.pd_test_enum_label((int)mirror.Bridge));
            var coreCount = TestNative.pd_test_cpp_enum_count((int)mirror.Bridge);
            var declared = Enum.GetValues(mirror.EnumType).Length;

            Assert.True(coreCount > 0, $"{label}: the bridge reported no members");
            Assert.True(mirror.HeaderCount == coreCount,
                        $"{label}: PD_*_COUNT says {mirror.HeaderCount}, the core says {coreCount}");
            Assert.True(declared == coreCount,
                        $"{label}: C# declares {declared} members, the core has {coreCount}. " +
                        "A member was added to the core and not mirrored here.");
        }
    }

    [Fact]
    public void MemberValuesAgreeWithTheCore()
    {
        foreach (var mirror in Mirrors)
        {
            var label = TestNative.ReadUtf8(TestNative.pd_test_enum_label((int)mirror.Bridge));
            var declared = Enum.GetValues(mirror.EnumType).Cast<object>()
                .Select(Convert.ToInt32).ToList();

            for (var index = 0; index < declared.Count; index++)
            {
                var expected = TestNative.pd_test_cpp_enum_value((int)mirror.Bridge, index);
                Assert.True(expected == declared[index],
                            $"{label}[{index}]: the core has {expected}, C# has {declared[index]}");
            }
        }
    }

    [Fact]
    public void MemberNamesAgreeWithTheCoresOwnSpelling()
    {
        foreach (var mirror in Mirrors.Where(m => m.HasComparableCoreNames))
        {
            var label = TestNative.ReadUtf8(TestNative.pd_test_enum_label((int)mirror.Bridge));
            var names = Enum.GetNames(mirror.EnumType);

            for (var index = 0; index < names.Length; index++)
            {
                var pointer = TestNative.pd_test_cpp_enum_name((int)mirror.Bridge, index);
                Assert.True(pointer != 0, $"{label}[{index}]: the core has no name for this");
                var core = TestNative.ReadUtf8(pointer);
                Assert.True(core == names[index],
                            $"{label}[{index}]: the core spells it '{core}', C# spells it " +
                            $"'{names[index]}'");
            }
        }
    }

    [Fact]
    public void TheAbiEnumNameHelpersAgreeWithTheCSharpNames()
    {
        // A second, independent route to the same answer: the shipped ABI's own
        // pd_*_name() functions rather than the test bridge.
        AssertNames<JobState>(NativeMethods.pd_job_state_name);
        AssertNames<ConfidenceLevel>(NativeMethods.pd_confidence_level_name);
        AssertNames<DeviceEvent>(NativeMethods.pd_device_event_name);
        AssertNames<FailureReason>(NativeMethods.pd_failure_reason_name);
        AssertNames<JobOutcome>(NativeMethods.pd_job_outcome_name);
        AssertNames<PayloadKind>(NativeMethods.pd_payload_kind_name);
        AssertNames<CompletionMechanism>(NativeMethods.pd_completion_mechanism_name);
        AssertNames<CutVariant>(NativeMethods.pd_cut_variant_name);
        AssertNames<CompletionAuthority>(NativeMethods.pd_completion_authority_name);

        // Provenance and CommandLanguage are not in the test bridge of
        // capi/tests/pd_test_support.h, so these helpers are the only independent
        // statement of what the core calls their members -- which is also the route an
        // application takes when it puts a grade or a language in front of a person.
        AssertNames<Provenance>(NativeMethods.pd_provenance_name);
        AssertNames<CommandLanguage>(NativeMethods.pd_command_language_name);
    }

    [Fact]
    public void TheGradeLadderGainedAPlusAtItsTop()
    {
        // docs/compatibility-brief.md §24. A+ took value 0 and pushed A..E down by one, so
        // the ladder still reads strongest-first and two grades can be compared
        // numerically. Anything that had hardcoded "A is 0" is wrong now, which is why
        // the values are asserted rather than assumed to have stayed put.
        Assert.Equal(0, (int)ConfidenceGrade.APlusDurableQueryableJob);
        Assert.Equal(1, (int)ConfidenceGrade.AJobLevelConfirmation);
        Assert.Equal(5, (int)ConfidenceGrade.ETransportOnly);
        Assert.Equal(6, Enum.GetValues<ConfidenceGrade>().Length);

        // The letter is what a report tabulates, and it is the one place A+ has to survive
        // as two characters rather than being rounded to "A".
        Assert.Equal(
            new[] { "A+", "A", "B", "C", "D", "E" },
            Enum.GetValues<ConfidenceGrade>()
                .Select(grade => TestNative.ReadUtf8(
                    NativeMethods.pd_confidence_grade_letter((int)grade)))
                .ToArray());

        // The core splits the grade letter off with an underscore
        // ("A_JobLevelConfirmation"); a C# member cannot. That underscore is the only
        // difference this compares away -- everything else has to match character for
        // character, so a member renamed in the core still fails here.
        foreach (var grade in Enum.GetValues<ConfidenceGrade>())
        {
            var core = TestNative.ReadUtf8(NativeMethods.pd_confidence_grade_name((int)grade));
            Assert.Equal(core.Replace("_", string.Empty), Enum.GetName(grade));
        }
    }

    [Fact]
    public void TheNonContiguousCodePageEnumEnumeratesThroughTheAbi()
    {
        // PD_CODE_PAGE_COUNT is a member count, not a past-the-end sentinel: the values
        // are the `n` operand of ESC t n and jump 0, 2, 16, 18, 19. pd_code_page_at is the
        // ABI's own iterator over them.
        var declared = Enum.GetValues<CodePage>();
        Assert.Equal(5, declared.Length);

        for (var index = 0; index < declared.Length; index++)
        {
            Assert.Equal((int)declared[index], NativeMethods.pd_code_page_at(index));
        }
        Assert.Equal(new[] { 0, 2, 16, 18, 19 }, declared.Select(v => (int)v).ToArray());
    }

    private static void AssertNames<TEnum>(Func<int, nint> nameOf) where TEnum : struct, Enum
    {
        foreach (var value in Enum.GetValues<TEnum>())
        {
            var native = TestNative.ReadUtf8(nameOf(Convert.ToInt32(value)));
            Assert.Equal(Enum.GetName(value), native);
        }
    }
}

/// <summary>
/// Struct layouts, which are the other half of the ABI contract the enums cover.
/// </summary>
/// <remarks>
/// Sizes are derived from capi/include/printerdriver/pd.h on a 64-bit target: pointers
/// and size_t are 8 bytes, enums are int. Getting one wrong does not fail loudly — it
/// reads the wrong bytes — so it is asserted directly here as well as being exercised by
/// every end-to-end test in PrintJobTests, each of which would produce nonsense states or
/// confidences if a field were misplaced.
/// </remarks>
public sealed class AbiLayoutTests
{
    [Fact]
    public void StructSizesMatchTheCHeader()
    {
        Assert.Equal(32, Marshal.SizeOf<PdConfig>());          // ptr, int+pad, ptr, ptr
        Assert.Equal(40, Marshal.SizeOf<PdTcpConfig>());       // ptr, ptr, u16+pad, u32+pad, ptr, u32+pad
        Assert.Equal(32, Marshal.SizeOf<PdTransportVtable>()); // 3 fn ptrs, ptr
        Assert.Equal(40, Marshal.SizeOf<PdJobOptions>());      // ptr, 7 x int, +pad
        Assert.Equal(24, Marshal.SizeOf<PdJobEvent>());        // 4 x int, u64
        Assert.Equal(32, Marshal.SizeOf<PdJobResult>());       // 5 x int, +pad, ptr
        Assert.Equal(36, Marshal.SizeOf<PdDeviceStatus>());    // 9 x int
        Assert.Equal(32, Marshal.SizeOf<PdRasterRgba8>());     // ptr, 3 x u32, int, u8+pad, u32
        Assert.Equal(24, Marshal.SizeOf<PdOp>());              // int+pad, ptr, int+pad
        Assert.Equal(24, Marshal.SizeOf<PdDocument>());        // ptr, size_t, int+pad
        Assert.Equal(16, Marshal.SizeOf<PdRaw>());             // ptr, size_t
        Assert.Equal(40, Marshal.SizeOf<PdPayload>());         // int+pad, then the union
    }

    [Fact]
    public void TheUnionOverlapsAllThreePayloadTiers()
    {
        // Every member of pd_payload's union starts at the same offset; the tag decides
        // which one is real. If C# laid these out sequentially instead, the ABI would read
        // a pointer out of padding.
        Assert.Equal(Marshal.SizeOf<PdRasterRgba8>(), Marshal.SizeOf<PdPayloadUnion>());
        Assert.Equal(0, Marshal.OffsetOf<PdPayload>(nameof(PdPayload.Kind)).ToInt32());
        Assert.Equal(8, Marshal.OffsetOf<PdPayload>(nameof(PdPayload.As)).ToInt32());
    }
}
