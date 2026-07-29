using System.IO;
using System.ComponentModel;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using CapturePanel.App.Models;
using CapturePanel.App.Services;
using CapturePanel.App.ViewModels;

namespace CapturePanel.App.Tests;

internal static class Program
{
    private static async Task<int> Main(string[] args)
    {
        if (args.Length > 0
            && Environment.GetEnvironmentVariable("CAPTURE_PANEL_TEST_WORKER_MODE") is { } workerMode)
        {
            return RunWorkerHelper(workerMode, args);
        }

        var tests = new (string Name, Func<Task> Run)[]
        {
            ("output device selection synchronizes the disabled input device", DeviceSelectionSynchronizesInput),
            ("source test capture gate and trim invalidation", SourceTestGateAndTrimInvalidation),
            ("setup input level uses the Mac verification boundaries", SetupInputLevelUsesMacBoundaries),
            ("clipping blocks capture and reports failed stability", ClippingBlocksCapture),
            ("changed or missing source files fail closed", SourceChangesFailClosed),
            ("source changes during Test or Capture discard stale results", SourceChangesDuringWorkerAreRejected),
            ("capture cannot overwrite its source WAV", CaptureCannotOverwriteSource),
            ("cancelled save dialog does not prepare a notification", CancelledSaveDialogDoesNotPrepareNotification),
            ("destination preparation failures send a notification", DestinationPreparationFailuresNotify),
            ("inconsistent setup-test results are rejected", InconsistentSetupResultIsRejected),
            ("capture uses a temporary output and promotes it on success", CapturePromotesTemporaryOutput),
            ("capture warnings remain visible after a successful save", CaptureWarningsRemainVisible),
            ("notification errors cannot turn a saved capture into a failure", NotificationErrorsAreNonFatal),
            ("failure notification errors preserve the original failure", FailureNotificationErrorsAreNonFatal),
            ("invalid capture results are never promoted", InvalidCaptureResultsAreRejected),
            ("late worker completion cannot win a capture cancellation", LateCaptureCompletionStaysCancelled),
            ("capture cancellation wins a concurrent worker error", CaptureCancellationWinsConcurrentWorkerError),
            ("capture failure invalidates setup and remains visible", CaptureFailureInvalidatesSetup),
            ("stale channel discovery cannot overwrite a newer device", StaleChannelDiscoveryIsIgnored),
            ("fallback channels are persisted after a driver change", FallbackChannelsArePersisted),
            ("device discovery timeout becomes a recoverable UI error", DeviceDiscoveryTimeoutIsRecoverable),
            ("channel discovery timeout invalidates the route", ChannelDiscoveryTimeoutInvalidatesRoute),
            ("setup test watchdog unlocks a hung worker", SetupTestWatchdogUnlocksUi),
            ("disposing during discovery cancels the worker", DisposeCancelsDeviceDiscovery),
            ("JSON settings persist the Windows route", JsonSettingsRoundTrip),
            ("WAV metadata reader reports source format and frames", WavMetadataRoundTrip),
            ("WAV metadata reader rejects malformed native-incompatible files", WavMetadataRejectsMalformedFiles),
            ("bundle layout separates binaries documents and licenses", BundleLayoutSeparatesSupportFiles),
            ("native JSON worker contract supports Fake test and capture", NativeWorkerJsonContract),
            ("native worker client serializes and bounds cancellation", WorkerClientSerializesAndCancels),
            ("Windows Shell notification ABI is available", WindowsShellNotificationAbiIsAvailable),
            ("Windows Shell notification callbacks are isolated by icon ID", WindowsShellNotificationCallbacksAreIsolated),
            ("Windows Shell notification content preserves the outcome", WindowsShellNotificationContentPreservesOutcome),
            ("Windows Fluent views load and lay out off-screen", WindowsFluentViewsLoad),
        };

        var failures = 0;
        foreach (var test in tests)
        {
            try
            {
                await test.Run();
                Console.WriteLine($"[PASS] {test.Name}");
            }
            catch (Exception exception)
            {
                failures++;
                Console.WriteLine($"[FAIL] {test.Name}: {exception.Message}");
            }
        }

        Console.WriteLine($"{tests.Length} managed test(s), {failures} failure(s)");
        return failures == 0 ? 0 : 1;
    }

    private static int RunWorkerHelper(string mode, string[] args)
    {
        if (mode is "inconsistent-test" or "failed-test-exit-zero" && args[0] == "test")
        {
            Console.WriteLine(JsonSerializer.Serialize(new Dictionary<string, object?>
            {
                ["protocol"] = "capture-panel/1",
                ["type"] = "test_result",
                ["passed"] = mode == "inconsistent-test",
                ["sampleRate"] = 48_000,
                ["outputPeakDbfs"] = -12,
                ["inputPeakDbfs"] = -18,
                ["alignment"] = new Dictionary<string, object?>
                {
                    ["markerLatencyFrames"] = 60,
                    ["markerLatencyMilliseconds"] = 1.25,
                },
                ["verification"] = new Dictionary<string, object?>
                {
                    ["timingFitErrorFrames"] = 4,
                    ["sweep"] = new Dictionary<string, object?> { ["reliability"] = "reliable" },
                },
                ["warnings"] = Array.Empty<object>(),
                ["failures"] = new[]
                {
                    new Dictionary<string, object?>
                    {
                        ["code"] = "digital_clipping",
                        ["message"] = "Clipping was detected.",
                    },
                },
            }));
            return 0;
        }
        if (mode == "capture-warning" && args[0] == "run")
        {
            var outputIndex = Array.IndexOf(args, "--output") + 1;
            var outputPath = args[outputIndex];
            Fixture.WritePcmWav(outputPath, sampleRate: 48_000, frames: 480);
            Console.WriteLine(JsonSerializer.Serialize(new Dictionary<string, object?>
            {
                ["protocol"] = "capture-panel/1",
                ["type"] = "event",
                ["event"] = "warning",
                ["warning"] = new Dictionary<string, object?>
                {
                    ["code"] = "source_near_digital_full_scale",
                    ["message"] = "The source is at or near digital full scale.",
                },
                ["message"] = "The selected source is at or near digital full scale.",
            }));
            Console.WriteLine(JsonSerializer.Serialize(new Dictionary<string, object?>
            {
                ["protocol"] = "capture-panel/1",
                ["type"] = "capture_result",
                ["output"] = new Dictionary<string, object?>
                {
                    ["path"] = outputPath,
                    ["fileSize"] = new FileInfo(outputPath).Length,
                    ["channelCount"] = 1,
                    ["bitDepth"] = 24,
                    ["sampleRate"] = 48_000,
                },
                ["alignment"] = new Dictionary<string, object?>
                {
                    ["markerLatencyFrames"] = 60,
                    ["markerLatencyMilliseconds"] = 1.25,
                    ["trimmedFrameCount"] = 480,
                    ["targetFrameCount"] = 480,
                },
                ["elapsedSeconds"] = 0.1,
            }));
            return 0;
        }
        if (args[0] != "devices")
        {
            return 2;
        }
        if (mode == "hang")
        {
            Thread.Sleep(Timeout.Infinite);
        }
        Thread.Sleep(300);
        Console.WriteLine(
            "{\"protocol\":\"capture-panel/1\",\"type\":\"devices\",\"devices\":[]}");
        return 0;
    }

    private static async Task DeviceSelectionSynchronizesInput()
    {
        using var fixture = new Fixture();
        using var model = fixture.CreateModel();
        await model.InitializeAsync();

        Require(model.SelectedOutputDevice is not null, "Expected a selected output device.");
        Require(ReferenceEquals(model.SelectedOutputDevice, model.SelectedInputDevice),
            "Recording device must be the exact synchronized playback selection.");
        Equal("asio:{FAKE-DRIVER}", model.SelectedInputDevice!.Id);
        Equal(2, model.PlaybackChannels.Count);
        Equal(2, model.RecordChannels.Count);
    }

    private static async Task SourceTestGateAndTrimInvalidation()
    {
        using var fixture = new Fixture();
        using var model = fixture.CreateModel();
        await model.InitializeAsync();
        model.ChooseSourceCommand.Execute(null);

        Require(model.SourceReady, "Source selection should be ready.");
        Require(model.CanTest, "Configured source and channels should enable Test.");
        Require(!model.CanCapture, "Capture must remain disabled before Test.");
        Equal(StatusKind.Neutral, model.StatusKind);

        string? lastNotifiedFooter = null;
        model.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName == nameof(MainViewModel.VerificationFooterText))
            {
                lastNotifiedFooter = model.VerificationFooterText;
            }
        };
        model.TestCommand.Execute(null);
        await WaitUntil(() => fixture.Worker.TestCalls == 1 && !model.IsTesting);
        Require(model.CaptureSetupVerified, "Passing Test should verify the current setup.");
        Require(model.CanCapture, "Passing Test should enable Capture.");
        Equal(CaptureStabilityLevel.Excellent, model.StabilityLevel);
        Equal(StatusKind.Ready, model.StatusKind);
        Equal("Excellent stability. Ready to capture.", model.VerificationFooterText);
        Equal(model.VerificationFooterText, lastNotifiedFooter);

        model.OutputTrimDb = -6;
        Require(!model.CaptureSetupVerified, "Trim changes must invalidate Test.");
        Require(!model.CanCapture, "Invalidated Test must disable Capture.");
        Equal("Output level changed. Test again.", model.Assessment);
        Equal("-", model.LevelText);
        Equal(SetupInputLevel.Unavailable, model.LevelState);
    }

    private static async Task SetupInputLevelUsesMacBoundaries()
    {
        var cases = new (double Peak, string Text, SetupInputLevel Level)[]
        {
            (-24.1, "Low", SetupInputLevel.Low),
            (-24.0, "Normal", SetupInputLevel.Normal),
            (-6.1, "Normal", SetupInputLevel.Normal),
            (-6.0, "Hot", SetupInputLevel.Hot),
        };

        foreach (var testCase in cases)
        {
            using var fixture = new Fixture();
            fixture.Worker.TestResult = fixture.Worker.TestResult with
            {
                InputPeakDbfs = testCase.Peak,
            };
            using var model = fixture.CreateModel();
            await model.InitializeAsync();
            model.ChooseSourceCommand.Execute(null);
            model.TestCommand.Execute(null);
            await WaitUntil(() => fixture.Worker.TestCalls == 1 && !model.IsTesting);

            Equal(testCase.Text, model.LevelText);
            Equal(testCase.Level, model.LevelState);
            Require(model.CanCapture, $"{testCase.Text} level must not block a passing setup test.");
        }
    }

    private static async Task ClippingBlocksCapture()
    {
        using var fixture = new Fixture();
        fixture.Worker.TestResult = fixture.Worker.TestResult with
        {
            Passed = false,
            InputPeakDbfs = -12,
            Failures = [new DiagnosticInfo("digital_clipping", "Clipping was detected.")],
        };
        using var model = fixture.CreateModel();
        await model.InitializeAsync();
        model.ChooseSourceCommand.Execute(null);
        model.TestCommand.Execute(null);
        await WaitUntil(() => fixture.Worker.TestCalls == 1 && !model.IsTesting);

        Require(!model.CanCapture, "Clipping must block Capture.");
        Equal(CaptureStabilityLevel.Failed, model.StabilityLevel);
        Equal(StatusKind.Error, model.StatusKind);
        Equal("Clipping", model.LevelText);
        Equal(SetupInputLevel.Clipping, model.LevelState);
        Require(model.IsClipping,
            "A raw clipping diagnostic must remain visible when negative input trim lowers the reported peak.");
        Equal("Clipping detected. Lower output or input, then test again.", model.Assessment);
    }

    private static async Task SourceChangesFailClosed()
    {
        using (var fixture = new Fixture())
        using (var model = fixture.CreateModel())
        {
            await model.InitializeAsync();
            model.ChooseSourceCommand.Execute(null);
            model.TestCommand.Execute(null);
            await WaitUntil(() => model.CanCapture);

            Fixture.WritePcmWav(fixture.SourcePath, sampleRate: 44_100, frames: 480);
            model.ChooseSourceCommand.Execute(null);

            Require(!model.CaptureSetupVerified,
                "Reselecting a changed source at the same path must invalidate Test.");
            Require(model.SourceDescription.StartsWith("44.1 kHz", StringComparison.Ordinal),
                "Reselecting should refresh the source metadata.");
        }

        using (var fixture = new Fixture())
        using (var model = fixture.CreateModel())
        {
            await model.InitializeAsync();
            model.ChooseSourceCommand.Execute(null);
            model.TestCommand.Execute(null);
            await WaitUntil(() => model.CanCapture);
            File.Delete(fixture.SourcePath);

            model.CaptureCommand.Execute(null);
            await WaitUntil(() => !model.IsCapturing && !model.SourceReady);

            Equal(0, fixture.Worker.CaptureCalls);
            Require(!model.CanCapture, "A missing source must disable Capture.");
            Require(model.Assessment.Contains("no longer available", StringComparison.Ordinal),
                "The missing-source reason should remain visible.");
            Equal(StatusKind.Error, model.StatusKind);
            Equal(model.Assessment, model.StatusMessage);
            Equal(1, fixture.Notifications.FailedCaptures.Count);
            Equal("source.wav", fixture.Notifications.FailedCaptures[0].Filename);
            Require(
                fixture.Notifications.FailedCaptures[0].Reason.Contains(
                    "no longer available",
                    StringComparison.Ordinal),
                "A missing source should provide the failure reason to the notification.");
        }
    }

    private static async Task SourceChangesDuringWorkerAreRejected()
    {
        using (var fixture = new Fixture())
        {
            fixture.Worker.HoldTestCompletion = true;
            using var model = fixture.CreateModel();
            await model.InitializeAsync();
            model.ChooseSourceCommand.Execute(null);

            model.TestCommand.Execute(null);
            await fixture.Worker.TestStarted.Task.WaitAsync(TimeSpan.FromSeconds(2));
            MutateWavPayload(fixture.SourcePath);
            fixture.Worker.ReleaseTestCompletion.TrySetResult();
            await WaitUntil(() => !model.IsTesting);

            Require(!model.CaptureSetupVerified,
                "A Test result must not verify a source that changed while the worker was running.");
            Require(model.Assessment.Contains("changed on disk", StringComparison.Ordinal),
                "The post-Test source change should remain visible.");
            Equal(StatusKind.Error, model.StatusKind);
        }

        using (var fixture = new Fixture())
        {
            using var model = fixture.CreateModel();
            await model.InitializeAsync();
            model.ChooseSourceCommand.Execute(null);
            model.TestCommand.Execute(null);
            await WaitUntil(() => model.CanCapture);
            fixture.Worker.HoldCaptureCompletion = true;

            model.CaptureCommand.Execute(null);
            await fixture.Worker.CaptureOutputWritten.Task.WaitAsync(TimeSpan.FromSeconds(2));
            MutateWavPayload(fixture.SourcePath);
            fixture.Worker.ReleaseCaptureCompletion.TrySetResult();
            await WaitUntil(() => !model.IsCapturing);

            Require(!File.Exists(fixture.DestinationPath),
                "A capture must not be promoted when its source changed during the worker operation.");
            Require(!model.CanCapture,
                "A mid-Capture source change must invalidate the previous setup verification.");
            Require(model.Assessment.Contains("changed on disk", StringComparison.Ordinal),
                "The post-Capture source change should remain visible.");
            Require(!Directory.EnumerateFiles(fixture.DirectoryPath, ".capture-panel-*.tmp.wav").Any(),
                "A discarded stale capture should remove its temporary output.");
            Equal(1, fixture.Notifications.FailedCaptures.Count);
            Require(
                fixture.Notifications.FailedCaptures[0].Reason.Contains(
                    "changed on disk",
                    StringComparison.Ordinal),
                "A changed source should produce a capture failure notification.");
        }
    }

    private static void MutateWavPayload(string path)
    {
        var bytes = File.ReadAllBytes(path);
        Require(bytes.Length > 44, "The source fixture has no audio payload to mutate.");
        bytes[44] ^= 0x01;
        File.WriteAllBytes(path, bytes);
    }

    private static async Task CaptureCannotOverwriteSource()
    {
        using var fixture = new Fixture();
        var dialogs = new FakeFileDialogService(fixture.SourcePath, fixture.SourcePath);
        var notifications = new RecordingCaptureNotificationService();
        using var model = new MainViewModel(
            fixture.Worker,
            fixture.Settings,
            dialogs,
            showDevelopmentDevices: true,
            notificationService: notifications);
        await model.InitializeAsync();
        model.ChooseSourceCommand.Execute(null);
        model.TestCommand.Execute(null);
        await WaitUntil(() => model.CanCapture);

        var originalLength = new FileInfo(fixture.SourcePath).Length;
        model.CaptureCommand.Execute(null);
        await WaitUntil(() => model.Assessment.Contains("must not overwrite", StringComparison.Ordinal));

        Equal(0, fixture.Worker.CaptureCalls);
        Equal(originalLength, new FileInfo(fixture.SourcePath).Length);
        Equal(480L, WavMetadataReader.Read(fixture.SourcePath).Frames);
        Require(model.CanCapture, "A rejected destination should preserve the verified audio route.");
        Equal(StatusKind.Error, model.StatusKind);
        Equal(1, notifications.FailedCaptures.Count);
        Equal(
            "The destination must not overwrite the source WAV.",
            notifications.FailedCaptures[0].Reason);

        using var hardLinkFixture = new Fixture();
        var hardLinkPath = Path.Combine(hardLinkFixture.DirectoryPath, "source-hard-link.wav");
        if (!CreateHardLink(hardLinkPath, hardLinkFixture.SourcePath, IntPtr.Zero))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not create the hard-link test fixture.");
        }
        var hardLinkDialogs = new FakeFileDialogService(hardLinkFixture.SourcePath, hardLinkPath);
        using var hardLinkModel = new MainViewModel(
            hardLinkFixture.Worker,
            hardLinkFixture.Settings,
            hardLinkDialogs,
            showDevelopmentDevices: true);
        await hardLinkModel.InitializeAsync();
        hardLinkModel.ChooseSourceCommand.Execute(null);
        hardLinkModel.TestCommand.Execute(null);
        await WaitUntil(() => hardLinkModel.CanCapture);
        hardLinkModel.CaptureCommand.Execute(null);
        await WaitUntil(() => hardLinkModel.Assessment.Contains("must not overwrite", StringComparison.Ordinal));
        Equal(0, hardLinkFixture.Worker.CaptureCalls);
    }

    private static async Task CancelledSaveDialogDoesNotPrepareNotification()
    {
        using var fixture = new Fixture();
        var notifications = new RecordingCaptureNotificationService();
        using var model = new MainViewModel(
            fixture.Worker,
            fixture.Settings,
            new FakeFileDialogService(fixture.SourcePath, destinationPath: null),
            showDevelopmentDevices: true,
            notificationService: notifications);
        await model.InitializeAsync();
        model.ChooseSourceCommand.Execute(null);
        model.TestCommand.Execute(null);
        await WaitUntil(() => model.CanCapture);

        model.CaptureCommand.Execute(null);

        Equal(0, fixture.Worker.CaptureCalls);
        Equal(0, notifications.PrepareCalls);
        Equal(0, notifications.NotifyCalls);
    }

    private static async Task DestinationPreparationFailuresNotify()
    {
        var dialogFactories = new Func<string, IFileDialogService>[]
        {
            sourcePath => new ThrowingFileDialogService(
                sourcePath,
                new IOException("The save dialog could not be opened.")),
            sourcePath => new FakeFileDialogService(
                sourcePath,
                "invalid\0destination.wav"),
        };

        foreach (var createDialogs in dialogFactories)
        {
            using var fixture = new Fixture();
            var notifications = new RecordingCaptureNotificationService();
            using var model = new MainViewModel(
                fixture.Worker,
                fixture.Settings,
                createDialogs(fixture.SourcePath),
                showDevelopmentDevices: true,
                notificationService: notifications);
            await model.InitializeAsync();
            model.ChooseSourceCommand.Execute(null);
            model.TestCommand.Execute(null);
            await WaitUntil(() => model.CanCapture);

            model.CaptureCommand.Execute(null);

            Equal(0, fixture.Worker.CaptureCalls);
            Require(model.CanCapture,
                "A destination error must preserve the verified audio route.");
            Require(model.Assessment.StartsWith("Capture failed:", StringComparison.Ordinal),
                "A destination error should remain visible.");
            Equal(1, notifications.PrepareCalls);
            Equal(1, notifications.FailedCaptures.Count);
            Equal("source.wav", notifications.FailedCaptures[0].Filename);
            Require(!string.IsNullOrWhiteSpace(notifications.FailedCaptures[0].Reason),
                "A destination failure notification must include a reason.");
        }
    }

    private static async Task InconsistentSetupResultIsRejected()
    {
        using (var fixture = new Fixture())
        {
            fixture.Worker.TestResult = fixture.Worker.TestResult with
            {
                Passed = true,
                Failures = [new DiagnosticInfo("digital_clipping", "Clipping was detected.")],
            };
            using var model = fixture.CreateModel();
            await model.InitializeAsync();
            model.ChooseSourceCommand.Execute(null);
            model.TestCommand.Execute(null);
            await WaitUntil(() => fixture.Worker.TestCalls == 1 && !model.IsTesting);

            Require(!model.CanCapture, "Failures must win over an inconsistent passed flag.");
            Equal(StatusKind.Error, model.StatusKind);
            Require(model.Assessment.Contains("disagrees", StringComparison.Ordinal),
                "The inconsistent result should be reported as a protocol failure.");
        }

        using (var fixture = new Fixture())
        {
            fixture.Worker.TestResult = fixture.Worker.TestResult with { SampleRate = double.NaN };
            using var model = fixture.CreateModel();
            await model.InitializeAsync();
            model.ChooseSourceCommand.Execute(null);
            model.TestCommand.Execute(null);
            await WaitUntil(() => fixture.Worker.TestCalls == 1 && !model.IsTesting);
            Require(!model.CanCapture, "Non-finite setup measurements must fail closed.");
        }
    }

    private static async Task CapturePromotesTemporaryOutput()
    {
        using var fixture = new Fixture();
        using var model = fixture.CreateModel();
        await model.InitializeAsync();
        model.ChooseSourceCommand.Execute(null);
        model.TestCommand.Execute(null);
        await WaitUntil(() => model.CanCapture);

        model.CaptureCommand.Execute(null);
        await WaitUntil(() => fixture.Worker.CaptureCalls == 1 && !model.IsCapturing);

        Equal(fixture.DestinationPath, model.LastOutputPath);
        Require(File.Exists(fixture.DestinationPath), "Final capture file was not promoted.");
        Require(!Directory.EnumerateFiles(fixture.DirectoryPath, ".*.tmp.wav").Any(),
            "Temporary capture output should be removed.");
        Equal("Capture saved.", model.Assessment);
        Equal("Saved source-captured.wav.", model.StatusMessage);
        Require(model.CanCapture, "A successful capture should preserve the verified setup.");
        Equal(1, fixture.Notifications.PrepareCalls);
        Equal(1, fixture.Notifications.SavedFilenames.Count);
        Equal("source-captured.wav", fixture.Notifications.SavedFilenames[0]);
        Equal(0, fixture.Notifications.FailedCaptures.Count);
    }

    private static async Task CaptureWarningsRemainVisible()
    {
        using var fixture = new Fixture();
        fixture.Worker.CaptureWarnings =
        [
            new DiagnosticInfo(
                "source_near_digital_full_scale",
                "The source is at or near digital full scale."),
        ];
        using var model = fixture.CreateModel();
        await model.InitializeAsync();
        model.ChooseSourceCommand.Execute(null);
        model.TestCommand.Execute(null);
        await WaitUntil(() => model.CanCapture);

        model.CaptureCommand.Execute(null);
        await WaitUntil(() => fixture.Worker.CaptureCalls == 1 && !model.IsCapturing);

        Equal(StatusKind.Warning, model.StatusKind);
        Equal("Saved with Warning", model.StatusTitle);
        Require(model.StatusMessage.Contains("digital full scale", StringComparison.Ordinal),
            "A successful capture warning must remain visible in the status card.");
        Require(model.CanCapture, "A capture warning should not invalidate a verified route.");
    }

    private static async Task NotificationErrorsAreNonFatal()
    {
        using var fixture = new Fixture();
        fixture.Notifications.ThrowOnPrepare = true;
        fixture.Notifications.ThrowOnNotify = true;
        using var model = fixture.CreateModel();
        await model.InitializeAsync();
        model.ChooseSourceCommand.Execute(null);
        model.TestCommand.Execute(null);
        await WaitUntil(() => model.CanCapture);

        model.CaptureCommand.Execute(null);
        await WaitUntil(() => fixture.Worker.CaptureCalls == 1 && !model.IsCapturing);

        Require(File.Exists(fixture.DestinationPath),
            "A notification error must not discard an already saved capture.");
        Equal("Capture saved.", model.Assessment);
        Equal("Saved source-captured.wav.", model.StatusMessage);
        Require(model.CanCapture,
            "A notification error must preserve the verified capture route.");
        Equal(1, fixture.Notifications.PrepareCalls);
        Equal(1, fixture.Notifications.NotifyCalls);
    }

    private static async Task FailureNotificationErrorsAreNonFatal()
    {
        using var fixture = new Fixture();
        fixture.Worker.CaptureException = new IOException("The ASIO device was disconnected.");
        fixture.Notifications.ThrowOnNotify = true;
        using var model = fixture.CreateModel();
        await model.InitializeAsync();
        model.ChooseSourceCommand.Execute(null);
        model.TestCommand.Execute(null);
        await WaitUntil(() => model.CanCapture);

        model.CaptureCommand.Execute(null);
        await WaitUntil(() => fixture.Worker.CaptureCalls == 1 && !model.IsCapturing);

        Require(!model.CanCapture,
            "A notification error must not restore a failed capture route.");
        Equal(CaptureStabilityLevel.Failed, model.StabilityLevel);
        Require(model.Assessment.Contains("disconnected", StringComparison.Ordinal),
            "The original capture failure must remain visible.");
        Equal(1, fixture.Notifications.NotifyCalls);
    }

    private static async Task InvalidCaptureResultsAreRejected()
    {
        var configureCases = new Action<FakeWorkerClient>[]
        {
            worker => worker.WriteCorruptCapture = true,
            worker => worker.CaptureFileSizeAdjustment = 1,
            worker => worker.CaptureResultPath = Path.Combine(Path.GetTempPath(), "wrong-capture.wav"),
            worker => worker.CaptureResultChannels = 2,
            worker =>
            {
                worker.CaptureFileChannels = 2;
                worker.CaptureResultChannels = 2;
            },
            worker =>
            {
                worker.CaptureFileBitDepth = 16;
                worker.CaptureResultBitDepth = 16;
            },
        };

        foreach (var configure in configureCases)
        {
            using var fixture = new Fixture();
            configure(fixture.Worker);
            using var model = fixture.CreateModel();
            await model.InitializeAsync();
            model.ChooseSourceCommand.Execute(null);
            model.TestCommand.Execute(null);
            await WaitUntil(() => model.CanCapture);

            model.CaptureCommand.Execute(null);
            await WaitUntil(() => fixture.Worker.CaptureCalls == 1 && !model.IsCapturing);

            Require(!File.Exists(fixture.DestinationPath),
                "An invalid worker result must never be promoted.");
            Require(!model.CanCapture, "Invalid output must invalidate the worker result.");
            Require(model.Assessment.StartsWith("Capture failed:", StringComparison.Ordinal),
                "Invalid output should produce a visible capture failure.");
            Require(!Directory.EnumerateFiles(fixture.DirectoryPath, ".capture-panel-*.tmp.wav").Any(),
                "Invalid temporary output should be removed.");
            Equal(1, fixture.Notifications.PrepareCalls);
            Equal(1, fixture.Notifications.NotifyCalls);
            Equal(1, fixture.Notifications.FailedCaptures.Count);
            Equal("source.wav", fixture.Notifications.FailedCaptures[0].Filename);
            Require(!string.IsNullOrWhiteSpace(fixture.Notifications.FailedCaptures[0].Reason),
                "An invalid worker result should include a failure reason.");
        }
    }

    private static async Task LateCaptureCompletionStaysCancelled()
    {
        using var fixture = new Fixture();
        fixture.Worker.HoldCaptureCompletion = true;
        fixture.Worker.CreateNativeSiblingTemporary = true;
        using var model = fixture.CreateModel();
        await model.InitializeAsync();
        model.ChooseSourceCommand.Execute(null);
        model.TestCommand.Execute(null);
        await WaitUntil(() => model.CanCapture);

        model.CaptureCommand.Execute(null);
        await fixture.Worker.CaptureOutputWritten.Task.WaitAsync(TimeSpan.FromSeconds(2));
        model.CaptureCommand.Execute(null);
        fixture.Worker.ReleaseCaptureCompletion.TrySetResult();
        await WaitUntil(() => !model.IsCapturing);
        await Task.Delay(100);

        Require(!File.Exists(fixture.DestinationPath),
            "A cancelled capture must never promote a late temporary result.");
        Require(!Directory.EnumerateFiles(fixture.DirectoryPath).Any(path =>
                Path.GetFileName(path).Contains(".capture-panel.tmp.", StringComparison.Ordinal)),
            "A cancelled native atomic write must not leave its inner sibling temp behind.");
        Equal("Capture cancelled.", model.Assessment);
        Equal("Cancelled", model.ProgressLabel);
        Require(model.ProgressFraction is null,
            "Queued progress from a cancelled worker must not revive the progress bar.");
        Require(model.CanCapture, "Cancellation should preserve the previously verified route.");
        Equal(1, fixture.Notifications.PrepareCalls);
        Equal(0, fixture.Notifications.NotifyCalls);
    }

    private static async Task CaptureFailureInvalidatesSetup()
    {
        using var fixture = new Fixture();
        fixture.Worker.CaptureException = new IOException("The ASIO device was disconnected.");
        using var model = fixture.CreateModel();
        await model.InitializeAsync();
        model.ChooseSourceCommand.Execute(null);
        model.TestCommand.Execute(null);
        await WaitUntil(() => model.CanCapture);

        model.CaptureCommand.Execute(null);
        await WaitUntil(() => fixture.Worker.CaptureCalls == 1 && !model.IsCapturing);

        Require(!model.CanCapture, "A worker failure must invalidate the verified setup.");
        Equal(CaptureStabilityLevel.Failed, model.StabilityLevel);
        Require(model.StatusMessage.Contains("disconnected", StringComparison.Ordinal),
            "The capture failure should remain visible in the status region.");
        Equal(model.StatusMessage, model.VerificationFooterText);
        Equal(1, fixture.Notifications.PrepareCalls);
        Equal(1, fixture.Notifications.NotifyCalls);
        Equal(1, fixture.Notifications.FailedCaptures.Count);
        Equal("source.wav", fixture.Notifications.FailedCaptures[0].Filename);
        Equal(
            "The ASIO device was disconnected.",
            fixture.Notifications.FailedCaptures[0].Reason);
    }

    private static async Task CaptureCancellationWinsConcurrentWorkerError()
    {
        using var fixture = new Fixture();
        fixture.Worker.CaptureCancellationRaceException =
            new IOException("The ASIO driver failed while the worker was stopping.");
        using var model = fixture.CreateModel();
        await model.InitializeAsync();
        model.ChooseSourceCommand.Execute(null);
        model.TestCommand.Execute(null);
        await WaitUntil(() => model.CanCapture);

        model.CaptureCommand.Execute(null);
        await WaitUntil(() => fixture.Worker.CaptureCalls == 1 && model.IsCapturing);
        model.CaptureCommand.Execute(null);
        await WaitUntil(() => !model.IsCapturing);

        Equal("Cancelled", model.ProgressLabel);
        Equal("Capture cancelled.", model.Assessment);
        Require(model.CanCapture,
            "A worker teardown error after cancellation must preserve the verified route.");
        Require(!File.Exists(fixture.DestinationPath),
            "A cancellation/error race must not promote an output file.");
        Require(model.RecoveryOutputPath is null,
            "An unvalidated cancellation/error race must not expose a recovery output.");
        Equal(0, fixture.Notifications.NotifyCalls);
    }

    private static async Task StaleChannelDiscoveryIsIgnored()
    {
        var worker = new RacingChannelWorkerClient();
        using var fixture = new Fixture();
        using var model = new MainViewModel(worker, new MemorySettingsStore(), fixture.Dialogs);
        await model.InitializeAsync();

        var deviceA = model.Devices.Single(device => device.Id == "asio:A");
        var deviceB = model.Devices.Single(device => device.Id == "asio:B");
        model.SelectedOutputDevice = deviceB;
        await worker.DeviceBRequested.Task.WaitAsync(TimeSpan.FromSeconds(2));
        model.SelectedOutputDevice = deviceA;
        await WaitUntil(() => model.PlaybackChannels.FirstOrDefault()?.Name == "A Output");

        worker.ReleaseDeviceB.TrySetResult();
        await Task.Delay(50);
        Equal("asio:A", model.SelectedOutputDevice?.Id);
        Equal("A Output", model.PlaybackChannels.Single().Name);
        Equal("A Input", model.RecordChannels.Single().Name);
    }

    private static async Task FallbackChannelsArePersisted()
    {
        var worker = new RacingChannelWorkerClient();
        var settings = new MemorySettingsStore();
        using var fixture = new Fixture();
        using var model = new MainViewModel(worker, settings, fixture.Dialogs);
        await model.InitializeAsync();

        model.SelectedOutputDevice = model.Devices.Single(device => device.Id == "asio:B");
        await worker.DeviceBRequested.Task.WaitAsync(TimeSpan.FromSeconds(2));
        worker.ReleaseDeviceB.TrySetResult();
        await WaitUntil(() => model.PlaybackChannels.FirstOrDefault()?.Name == "B Output");

        Equal(7, model.SelectedPlaybackChannel?.Index);
        Equal(7, model.SelectedRecordChannel?.Index);
        Equal(7, settings.Load().PlaybackChannel);
        Equal(7, settings.Load().RecordChannel);
    }

    private static async Task DeviceDiscoveryTimeoutIsRecoverable()
    {
        using var fixture = new Fixture();
        using var model = new MainViewModel(
            new TimeoutWorkerClient(timeoutDevices: true),
            new MemorySettingsStore(),
            fixture.Dialogs);

        await model.InitializeAsync();
        Require(!model.CanCapture, "A discovery timeout must keep Capture disabled.");
        Equal(StatusKind.Error, model.StatusKind);
        Require(model.Assessment.Contains("timed out", StringComparison.Ordinal),
            "The timeout should be retained as the recovery guidance.");
    }

    private static async Task ChannelDiscoveryTimeoutInvalidatesRoute()
    {
        using var fixture = new Fixture();
        using var model = new MainViewModel(
            new TimeoutWorkerClient(timeoutDevices: false),
            new MemorySettingsStore(),
            fixture.Dialogs);

        await model.InitializeAsync();
        Require(model.SelectedOutputDevice is not null, "The ASIO driver should still be visible.");
        Require(!model.AudioReady, "A channel timeout must leave the route unavailable.");
        Require(!model.CaptureSetupVerified, "A channel timeout must invalidate setup verification.");
        Require(model.Assessment.Contains("channel discovery timed out", StringComparison.Ordinal),
            "The route should explain how to retry after a channel timeout.");
    }

    private static async Task SetupTestWatchdogUnlocksUi()
    {
        using var fixture = new Fixture();
        fixture.Worker.HangTest = true;
        using var model = new MainViewModel(
            fixture.Worker,
            fixture.Settings,
            fixture.Dialogs,
            showDevelopmentDevices: true,
            setupTestTimeout: TimeSpan.FromMilliseconds(50));
        await model.InitializeAsync();
        model.ChooseSourceCommand.Execute(null);

        model.TestCommand.Execute(null);
        await WaitUntil(() => fixture.Worker.TestCalls == 1 && !model.IsTesting);

        Require(!model.ControlsLocked, "A timed-out Test must unlock configuration controls.");
        Require(!model.CanCapture, "A timed-out Test must not enable Capture.");
        Equal(CaptureStabilityLevel.Failed, model.StabilityLevel);
        Require(model.StatusMessage.Contains("timed out", StringComparison.Ordinal),
            "The Test watchdog should leave a visible timeout diagnostic.");
    }

    private static async Task DisposeCancelsDeviceDiscovery()
    {
        using var fixture = new Fixture();
        var worker = new HangingDiscoveryWorkerClient();
        var model = new MainViewModel(worker, new MemorySettingsStore(), fixture.Dialogs);
        var initialization = model.InitializeAsync();
        await worker.Started.Task.WaitAsync(TimeSpan.FromSeconds(2));

        model.Dispose();
        await initialization.WaitAsync(TimeSpan.FromSeconds(2));
        Require(worker.CancellationObserved, "Closing the UI must cancel its discovery worker.");
    }

    private static Task JsonSettingsRoundTrip()
    {
        using var fixture = new Fixture();
        var path = Path.Combine(fixture.DirectoryPath, "settings.json");
        var store = new JsonAppSettingsStore(path);
        var expected = new AppSettings(
            SourcePath: fixture.SourcePath,
            Device: new SavedDevice("asio:{FAKE-DRIVER}", "Fake ASIO"),
            PlaybackChannel: 2,
            RecordChannel: 1,
            OutputTrimDb: -12,
            InputTrimDb: 3);
        store.Save(expected);
        Equal(expected, store.Load());
        return Task.CompletedTask;
    }

    private static async Task WavMetadataRoundTrip()
    {
        using var fixture = new Fixture();
        var metadata = WavMetadataReader.Read(fixture.SourcePath);
        Equal(48_000, metadata.SampleRate);
        Equal(1, metadata.Channels);
        Equal(24, metadata.BitsPerSample);
        Equal(480L, metadata.Frames);

        var metadataOnly = WavMetadataReader.ReadMetadataSnapshot(fixture.SourcePath);
        Equal(string.Empty, metadataOnly.ContentSha256);
        var fingerprinted = await WavMetadataReader.ReadSnapshotAsync(
            fixture.SourcePath,
            CancellationToken.None);
        Equal(64, fingerprinted.ContentSha256.Length);
        Equal(metadataOnly.Metadata, fingerprinted.Metadata);

        using var cancelled = new CancellationTokenSource();
        cancelled.Cancel();
        await ExpectThrowsAsync<OperationCanceledException>(
            () => WavMetadataReader.ReadSnapshotAsync(fixture.SourcePath, cancelled.Token));
    }

    private static Task WavMetadataRejectsMalformedFiles()
    {
        using var fixture = new Fixture();

        var blockAlignPath = Path.Combine(fixture.DirectoryPath, "bad-block-align.wav");
        var blockAlignBytes = File.ReadAllBytes(fixture.SourcePath);
        blockAlignBytes[32] = 2;
        blockAlignBytes[33] = 0;
        File.WriteAllBytes(blockAlignPath, blockAlignBytes);
        ExpectThrows<InvalidDataException>(() => WavMetadataReader.Read(blockAlignPath));

        var partialFramePath = Path.Combine(fixture.DirectoryPath, "partial-frame.wav");
        var partialFrameBytes = File.ReadAllBytes(fixture.SourcePath);
        BitConverter.GetBytes(1_439).CopyTo(partialFrameBytes, 40);
        File.WriteAllBytes(partialFramePath, partialFrameBytes);
        ExpectThrows<InvalidDataException>(() => WavMetadataReader.Read(partialFramePath));

        var badExtensiblePath = Path.Combine(fixture.DirectoryPath, "bad-extensible.wav");
        Fixture.WriteExtensiblePcm24Wav(badExtensiblePath, validGuid: false);
        ExpectThrows<InvalidDataException>(() => WavMetadataReader.Read(badExtensiblePath));

        var validExtensiblePath = Path.Combine(fixture.DirectoryPath, "valid-extensible.wav");
        Fixture.WriteExtensiblePcm24Wav(validExtensiblePath, validGuid: true);
        Equal(1L, WavMetadataReader.Read(validExtensiblePath).Frames);

        var emptyPath = Path.Combine(fixture.DirectoryPath, "empty.wav");
        Fixture.WritePcmWav(emptyPath, sampleRate: 48_000, frames: 0);
        ExpectThrows<InvalidDataException>(() => WavMetadataReader.Read(emptyPath));

        var unsupportedRatePath = Path.Combine(fixture.DirectoryPath, "unsupported-rate.wav");
        Fixture.WritePcmWav(unsupportedRatePath, sampleRate: 800, frames: 1);
        ExpectThrows<InvalidDataException>(() => WavMetadataReader.ReadSnapshot(unsupportedRatePath));
        return Task.CompletedTask;
    }

    private static Task BundleLayoutSeparatesSupportFiles()
    {
        var expectedBin = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "bin"));
        var expectedDocs = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "docs"));
        var expectedLicenses = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "licenses"));
        Equal(expectedBin, Path.GetFullPath(AppBundlePaths.BinDirectory));
        Equal(expectedDocs, Path.GetFullPath(AppBundlePaths.DocsDirectory));
        Equal(expectedLicenses, Path.GetFullPath(AppBundlePaths.LicensesDirectory));
        Equal(
            Path.Combine(expectedBin, "capture-panel.exe"),
            Path.GetFullPath(AppBundlePaths.WorkerPath));
        Require(File.Exists(AppBundlePaths.WorkerPath), "The native worker was not copied under bin.");
        var readmePath = AppBundlePaths.DocumentPath("README.md");
        Require(File.Exists(readmePath), "The bundled README is missing.");
        var readme = File.ReadAllText(readmePath);
        Require(
            readme.Contains("../licenses/GPL-3.0.txt", StringComparison.Ordinal),
            "The bundled README does not link to the bundled GPL license.");
        Require(
            readme.Contains("../licenses/THIRD_PARTY_NOTICES.md", StringComparison.Ordinal),
            "The bundled README does not link to the bundled third-party notices.");

        var settings = new SettingsViewModel(AppBundlePaths.WorkerPath);
        Require(settings.LicenseAvailable, "The bundled GPL license is missing.");
        Require(settings.ThirdPartyNoticesAvailable, "The bundled third-party notices are missing.");
        Require(settings.AsioLicenseAvailable, "The bundled ASIO SDK license is missing.");
        return Task.CompletedTask;
    }

    private static async Task NativeWorkerJsonContract()
    {
        using var fixture = new Fixture();
        var worker = new CaptureWorkerClient(AppBundlePaths.WorkerPath);
        Require(worker.WorkerAvailable, "The native worker was not copied under bin.");

        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        var channels = await worker.GetChannelsAsync("fake:loopback", timeout.Token);
        Equal(8, channels.Inputs.Count);
        Equal(8, channels.Outputs.Count);

        var testResult = await worker.TestAsync(
            new SetupTestRequest("fake:loopback", 1, 1, 0, 0, 48_000),
            progress: null,
            timeout.Token);
        Require(testResult.Passed, "The native Fake setup test should pass.");
        Equal("reliable", testResult.Reliability);

        var clippingResult = await worker.TestAsync(
            new SetupTestRequest("fake:loopback", 1, 1, 0, 12, 48_000),
            progress: null,
            timeout.Token);
        Require(!clippingResult.Passed
                && clippingResult.Failures.Any(failure => failure.Code == "digital_clipping"),
            "The worker client must accept the documented exit code for a valid failed Test result.");

        var outputPath = Path.Combine(fixture.DirectoryPath, "native-capture.wav");
        var captureResult = await worker.CaptureAsync(
            new CaptureRequest(fixture.SourcePath, outputPath, "fake:loopback", 1, 1, 0, 0),
            progress: null,
            timeout.Token);
        Require(File.Exists(outputPath), "The native Fake capture did not write an output WAV.");
        Equal(Path.GetFullPath(outputPath), Path.GetFullPath(captureResult.OutputPath));
        Equal(480L, WavMetadataReader.Read(outputPath).Frames);
    }

    private static async Task WorkerClientSerializesAndCancels()
    {
        var helperPath = Path.Combine(AppContext.BaseDirectory, "CapturePanel.App.Tests.exe");
        Require(File.Exists(helperPath), "The managed worker helper apphost is missing.");
        var worker = new CaptureWorkerClient(helperPath);

        try
        {
            Environment.SetEnvironmentVariable("CAPTURE_PANEL_TEST_WORKER_MODE", "serial");
            var stopwatch = System.Diagnostics.Stopwatch.StartNew();
            await Task.WhenAll(
                worker.GetDevicesAsync(CancellationToken.None),
                worker.GetDevicesAsync(CancellationToken.None));
            stopwatch.Stop();
            Require(stopwatch.Elapsed >= TimeSpan.FromMilliseconds(500),
                "One CaptureWorkerClient must never overlap native worker processes.");

            Environment.SetEnvironmentVariable("CAPTURE_PANEL_TEST_WORKER_MODE", "hang");
            using var cancellation = new CancellationTokenSource(TimeSpan.FromMilliseconds(100));
            stopwatch.Restart();
            await ExpectThrowsAsync<OperationCanceledException>(
                () => worker.GetDevicesAsync(cancellation.Token));
            stopwatch.Stop();
            Require(stopwatch.Elapsed < TimeSpan.FromSeconds(5),
                "Cancelling a hung native worker must complete within the bounded cleanup window.");

            Environment.SetEnvironmentVariable("CAPTURE_PANEL_TEST_WORKER_MODE", "inconsistent-test");
            CaptureWorkerException? protocolError = null;
            try
            {
                _ = await worker.TestAsync(
                    new SetupTestRequest("fake:loopback", 1, 1, 0, 0, 48_000),
                    progress: null,
                    CancellationToken.None);
            }
            catch (CaptureWorkerException exception)
            {
                protocolError = exception;
            }
            Equal("protocol_error", protocolError?.Code);

            Environment.SetEnvironmentVariable("CAPTURE_PANEL_TEST_WORKER_MODE", "failed-test-exit-zero");
            protocolError = null;
            try
            {
                _ = await worker.TestAsync(
                    new SetupTestRequest("fake:loopback", 1, 1, 0, 0, 48_000),
                    progress: null,
                    CancellationToken.None);
            }
            catch (CaptureWorkerException exception)
            {
                protocolError = exception;
            }
            Equal("protocol_error", protocolError?.Code);

            Environment.SetEnvironmentVariable("CAPTURE_PANEL_TEST_WORKER_MODE", "capture-warning");
            using var fixture = new Fixture();
            var helperOutput = Path.Combine(fixture.DirectoryPath, "helper-output.wav");
            var capture = await worker.CaptureAsync(
                new CaptureRequest(fixture.SourcePath, helperOutput, "fake:loopback", 1, 1, 0, 0),
                progress: null,
                CancellationToken.None);
            Equal(1, capture.Warnings.Count);
            Equal("source_near_digital_full_scale", capture.Warnings[0].Code);
            Require(capture.Warnings[0].Message.Contains("selected source", StringComparison.Ordinal),
                "Capture warning collection should preserve the detailed event message.");
        }
        finally
        {
            Environment.SetEnvironmentVariable("CAPTURE_PANEL_TEST_WORKER_MODE", null);
        }
    }

    private static Task WindowsShellNotificationAbiIsAvailable()
    {
        var dataType = typeof(WindowsCaptureNotificationService).GetNestedType(
            "NotifyIconData",
            BindingFlags.NonPublic)
            ?? throw new InvalidOperationException("The Shell notification data type is missing.");
        Equal(976, Marshal.SizeOf(dataType));

        Require(NativeLibrary.TryLoad("shell32.dll", out var shell),
            "Windows Shell could not be loaded.");
        try
        {
            Require(NativeLibrary.TryGetExport(shell, "Shell_NotifyIconW", out _),
                "Shell_NotifyIconW is unavailable.");
            Require(NativeLibrary.TryGetExport(shell, "ExtractIconExW", out _),
                "ExtractIconExW is unavailable.");
        }
        finally
        {
            NativeLibrary.Free(shell);
        }

        return Task.CompletedTask;
    }

    private static Task WindowsShellNotificationCallbacksAreIsolated()
    {
        using var service = new WindowsCaptureNotificationService(
            System.Windows.Threading.Dispatcher.CurrentDispatcher);
        var serviceType = typeof(WindowsCaptureNotificationService);
        var iconIdField = serviceType.GetField(
            "_notificationIconId",
            BindingFlags.Instance | BindingFlags.NonPublic)
            ?? throw new InvalidOperationException("The notification icon ID field is missing.");
        var decodeMethod = serviceType.GetMethod(
            "TryDecodeCurrentNotificationCallback",
            BindingFlags.Instance | BindingFlags.NonPublic)
            ?? throw new InvalidOperationException("The notification callback decoder is missing.");
        iconIdField.SetValue(service, (ushort)2);

        object?[] staleVersion4Arguments =
        [
            IntPtr.Zero,
            new IntPtr((1 << 16) | 0x0403),
            0u,
        ];
        Require(!(bool)decodeMethod.Invoke(service, staleVersion4Arguments)!,
            "A delayed callback from an old icon must not close the current notification.");

        object?[] currentVersion4Arguments =
        [
            IntPtr.Zero,
            new IntPtr((2 << 16) | 0x0403),
            0u,
        ];
        Require((bool)decodeMethod.Invoke(service, currentVersion4Arguments)!,
            "The current version-4 Shell callback was not recognized.");
        Equal(0x0403u, (uint)currentVersion4Arguments[2]!);

        object?[] currentLegacyArguments =
        [
            new IntPtr(2),
            new IntPtr(0x0404),
            0u,
        ];
        Require((bool)decodeMethod.Invoke(service, currentLegacyArguments)!,
            "The current legacy Shell callback was not recognized.");
        Equal(0x0404u, (uint)currentLegacyArguments[2]!);

        return Task.CompletedTask;
    }

    private static Task WindowsShellNotificationContentPreservesOutcome()
    {
        var serviceType = typeof(WindowsCaptureNotificationService);
        var savedInfoMethod = serviceType.GetMethod(
            "BuildCaptureSavedInfo",
            BindingFlags.Static | BindingFlags.NonPublic)
            ?? throw new InvalidOperationException("The saved-notification formatter is missing.");
        var failedInfoMethod = serviceType.GetMethod(
            "BuildCaptureFailedInfo",
            BindingFlags.Static | BindingFlags.NonPublic)
            ?? throw new InvalidOperationException("The failed-notification formatter is missing.");

        var longFilename = new string('x', 255);
        var savedInfo = (string)savedInfoMethod.Invoke(null, [longFilename])!;
        Require(savedInfo.Length <= 255,
            "Saved notification content exceeds the Shell limit.");
        Require(savedInfo.EndsWith(" was saved successfully.", StringComparison.Ordinal),
            "A long filename must not remove the saved outcome.");

        const string reason = "The ASIO device was disconnected.";
        var failedInfo = (string)failedInfoMethod.Invoke(null, [longFilename, reason])!;
        Require(failedInfo.Length <= 255,
            "Failed notification content exceeds the Shell limit.");
        Require(failedInfo.Contains(reason, StringComparison.Ordinal),
            "A long filename must not remove the capture failure reason.");

        return Task.CompletedTask;
    }

    private static Task WindowsFluentViewsLoad()
        => RunOnSta(() =>
        {
            var application = new App();
            application.InitializeComponent();
            using var fixture = new Fixture();
            using var model = fixture.CreateModel();
            model.InitializeAsync().GetAwaiter().GetResult();

            var mainWindow = new MainWindow(model);
            var mainView = (FrameworkElement)mainWindow.Content;
            Equal(600d, mainWindow.Width);
            Equal(740d, mainWindow.Height);
            Equal(mainWindow.MinWidth, mainWindow.MaxWidth);
            Equal(mainWindow.MinHeight, mainWindow.MaxHeight);
            Equal(ResizeMode.CanMinimize, mainWindow.ResizeMode);
            RenderOffScreen(mainView, 600, 740, "main-window.png");
            var sourceDisplay = (Border?)mainWindow.FindName("SourceDisplay")
                ?? throw new InvalidOperationException("The source display was not found.");
            Require(!sourceDisplay.Focusable
                    && !FindVisualChildren<TextBox>(sourceDisplay).Any(),
                "The source filename must be display-only, not a selectable read-only text field.");
            var fluentIcons = FindVisualChildren<TextBlock>(mainView)
                .Where(control => control.FontFamily.Source.Contains(
                    "Segoe Fluent Icons", StringComparison.Ordinal))
                .ToArray();
            var expectedGlyphs = new[]
            {
                "\uE713", "\uE72C", "\uE768", "\uE7C8",
            };
            var semanticIcons = fluentIcons
                .Where(icon => icon.Text != "\uE70D")
                .ToArray();
            Require(semanticIcons.Length == 4
                    && expectedGlyphs.All(glyph => semanticIcons.Any(icon => icon.Text == glyph)),
                $"Only Settings, Refresh, Test, and Capture should display Fluent glyphs; found "
                + $"{semanticIcons.Length}: {string.Join(", ", semanticIcons.Select(icon => $"U+{(int)icon.Text[0]:X4}"))}.");
            Require(fluentIcons.All(icon => string.IsNullOrEmpty(AutomationProperties.GetName(icon))),
                "Decorative glyphs must not add duplicate screen-reader labels.");
            var statusCard = (Border?)mainWindow.FindName("StatusCard")
                ?? throw new InvalidOperationException("The status card was not found.");
            var statusDot = (System.Windows.Shapes.Ellipse?)mainWindow.FindName("StatusDot")
                ?? throw new InvalidOperationException("The status dot was not found.");
            var statusDotCenter = statusDot.TranslatePoint(
                new Point(statusDot.ActualWidth / 2, statusDot.ActualHeight / 2),
                statusCard);
            Require(Math.Abs(statusDotCenter.Y - (statusCard.ActualHeight / 2)) <= 1,
                "The status dot must remain vertically centered in the complete status card.");
            var statusTriggers = statusCard.Style.Triggers.OfType<DataTrigger>().ToArray();
            Require(statusTriggers.Length == 4
                    && statusTriggers.All(trigger => trigger.Setters.OfType<Setter>()
                        .All(setter => setter.Property != Border.BackgroundProperty))
                    && statusTriggers.All(trigger => trigger.Setters.OfType<Setter>()
                        .Any(setter => setter.Property == Border.BorderBrushProperty)),
                "Status states must change the card border, not its background.");
            var operationProgress = FindVisualChildren<ProgressBar>(mainView)
                .Single(control => AutomationProperties.GetName(control) == "Operation progress");
            var idleStatusHeight = statusCard.ActualHeight;
            operationProgress.Value = 80;
            operationProgress.Visibility = Visibility.Visible;
            mainView.UpdateLayout();
            Equal(idleStatusHeight, statusCard.ActualHeight);
            Require(operationProgress.ActualHeight >= idleStatusHeight - 5,
                "The operation progress background must cover the status card without adding height.");
            operationProgress.ApplyTemplate();
            var progressIndicator = (FrameworkElement?)operationProgress.Template.FindName(
                "PART_Indicator", operationProgress);
            Require(progressIndicator is not null
                    && Math.Abs((progressIndicator.ActualWidth / operationProgress.ActualWidth) - 0.8) < 0.02,
                "The status card progress background must represent the operation percentage.");
            RenderOffScreen(mainView, 600, 740, "main-window-progress.png");
            operationProgress.ClearValue(ProgressBar.ValueProperty);
            operationProgress.ClearValue(UIElement.VisibilityProperty);
            mainView.UpdateLayout();
            var captureButton = FindVisualChildren<Button>(mainView)
                .Single(control => AutomationProperties.GetName(control) == "Capture");
            Require(ReferenceEquals(application.FindResource("CaptureButtonStyle"), captureButton.Style),
                "Capture must use the dedicated high-emphasis action style.");
            Require(!ReferenceEquals(application.FindResource("AccentButtonStyle"), captureButton.Style.BasedOn),
                "Capture hover must not inherit the system accent-blue visual states.");
            var captureHoverTrigger = captureButton.Template.Triggers
                .OfType<Trigger>()
                .Single(trigger => trigger.Property == UIElement.IsMouseOverProperty);
            Require(captureHoverTrigger.Setters.OfType<Setter>()
                    .All(setter => setter.Property != Control.BackgroundProperty),
                "Capture hover must preserve the red fill instead of swapping colors.");
            captureButton.ApplyTemplate();
            Require(captureButton.Template.FindName("FocusBorder", captureButton) is null
                    && captureButton.FocusVisualStyle is not null,
                "Capture must use the keyboard-only system focus visual, not a persistent white border.");
            var captureCommand = captureButton.Command;
            captureButton.Command = null;
            captureButton.IsEnabled = true;
            mainView.UpdateLayout();
            Require(ReferenceEquals(
                    application.FindResource("CaptureFillColorBrush"),
                    captureButton.Background),
                "Enabled Capture must use the critical red fill.");
            RenderOffScreen(mainView, 600, 740, "main-window-capture-ready.png");
            captureButton.Command = captureCommand;
            captureButton.ClearValue(UIElement.IsEnabledProperty);
            mainView.UpdateLayout();
            var recordingDevice = (Border?)mainWindow.FindName("RecordingDeviceDisplay")
                ?? throw new InvalidOperationException("The Recording driver display was not found.");
            Require(!recordingDevice.Focusable
                    && !FindVisualChildren<ComboBox>(recordingDevice).Any(),
                "The linked Recording driver must be display-only with no dropdown affordance.");
            var recordingDeviceText = FindVisualChildren<TextBlock>(recordingDevice).Single();
            Equal(model.SelectedInputDevice?.DisplayName, recordingDeviceText.Text);
            RenderOffScreen(mainView, 600, 440, "main-window-compact.png");

            var settingsWindow = new SettingsWindow(new SettingsViewModel(model.WorkerPath));
            RenderOffScreen((FrameworkElement)settingsWindow.Content, 560, 500, "about-window.png");
        });

    private static Task RunOnSta(Action action)
    {
        var completion = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var thread = new Thread(() =>
        {
            try
            {
                action();
                completion.TrySetResult();
            }
            catch (Exception exception)
            {
                completion.TrySetException(exception);
            }
        });
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        return completion.Task;
    }

    private static void RenderOffScreen(FrameworkElement view, int width, int height, string snapshotName)
    {
        var size = new Size(width, height);
        view.Measure(size);
        view.Arrange(new Rect(size));
        view.UpdateLayout();
        var bitmap = new RenderTargetBitmap(width, height, 96, 96, PixelFormats.Pbgra32);
        bitmap.Render(view);
        Require(view.ActualWidth > 0 && view.ActualHeight > 0, "The WPF view did not complete layout.");

        var snapshotDirectory = Environment.GetEnvironmentVariable("CAPTURE_PANEL_UI_SNAPSHOT_DIR");
        if (!string.IsNullOrWhiteSpace(snapshotDirectory))
        {
            Directory.CreateDirectory(snapshotDirectory);
            var encoder = new PngBitmapEncoder();
            encoder.Frames.Add(BitmapFrame.Create(bitmap));
            using var stream = File.Create(Path.Combine(snapshotDirectory, snapshotName));
            encoder.Save(stream);
        }
    }

    private static IEnumerable<T> FindVisualChildren<T>(DependencyObject root) where T : DependencyObject
    {
        for (var index = 0; index < VisualTreeHelper.GetChildrenCount(root); index++)
        {
            var child = VisualTreeHelper.GetChild(root, index);
            if (child is T match)
            {
                yield return match;
            }
            foreach (var descendant in FindVisualChildren<T>(child))
            {
                yield return descendant;
            }
        }
    }

    private static async Task WaitUntil(Func<bool> predicate, int timeoutMilliseconds = 4_000)
    {
        var deadline = DateTime.UtcNow.AddMilliseconds(timeoutMilliseconds);
        while (!predicate())
        {
            if (DateTime.UtcNow >= deadline)
            {
                throw new TimeoutException("Timed out waiting for the view-model state transition.");
            }
            await Task.Delay(10);
        }
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }

    private static void Equal<T>(T expected, T actual)
    {
        if (!EqualityComparer<T>.Default.Equals(expected, actual))
        {
            throw new InvalidOperationException($"Expected '{expected}', received '{actual}'.");
        }
    }

    private static void ExpectThrows<TException>(Action action) where TException : Exception
    {
        try
        {
            action();
        }
        catch (TException)
        {
            return;
        }
        throw new InvalidOperationException($"Expected {typeof(TException).Name}.");
    }

    private static async Task ExpectThrowsAsync<TException>(Func<Task> action) where TException : Exception
    {
        try
        {
            await action();
        }
        catch (TException)
        {
            return;
        }
        throw new InvalidOperationException($"Expected {typeof(TException).Name}.");
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateHardLink(
        string fileName,
        string existingFileName,
        IntPtr securityAttributes);
}

internal sealed class Fixture : IDisposable
{
    public Fixture()
    {
        DirectoryPath = Path.Combine(Path.GetTempPath(), "capture-panel-ui-tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(DirectoryPath);
        SourcePath = Path.Combine(DirectoryPath, "source.wav");
        DestinationPath = Path.Combine(DirectoryPath, "source-captured.wav");
        WritePcmWav(SourcePath, sampleRate: 48_000, frames: 480);
        Worker = new FakeWorkerClient();
        Dialogs = new FakeFileDialogService(SourcePath, DestinationPath);
        Settings = new MemorySettingsStore();
        Notifications = new RecordingCaptureNotificationService();
    }

    public string DirectoryPath { get; }
    public string SourcePath { get; }
    public string DestinationPath { get; }
    public FakeWorkerClient Worker { get; }
    public FakeFileDialogService Dialogs { get; }
    public MemorySettingsStore Settings { get; }
    public RecordingCaptureNotificationService Notifications { get; }

    public MainViewModel CreateModel()
        => new(
            Worker,
            Settings,
            Dialogs,
            notificationService: Notifications,
            showDevelopmentDevices: true);

    public void Dispose()
    {
        if (Directory.Exists(DirectoryPath))
        {
            Directory.Delete(DirectoryPath, recursive: true);
        }
    }

    internal static void WritePcmWav(
        string path,
        int sampleRate,
        int frames,
        short channels = 1,
        short bitDepth = 24)
    {
        if (channels <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(channels));
        }
        if (bitDepth is not (16 or 24 or 32))
        {
            throw new ArgumentOutOfRangeException(nameof(bitDepth));
        }
        var blockAlign = checked((short)(channels * (bitDepth / 8)));
        var dataBytes = frames * blockAlign;
        using var stream = File.Create(path);
        using var writer = new BinaryWriter(stream);
        writer.Write("RIFF"u8.ToArray());
        writer.Write(36 + dataBytes);
        writer.Write("WAVE"u8.ToArray());
        writer.Write("fmt "u8.ToArray());
        writer.Write(16);
        writer.Write((short)1);
        writer.Write(channels);
        writer.Write(sampleRate);
        writer.Write(sampleRate * blockAlign);
        writer.Write(blockAlign);
        writer.Write(bitDepth);
        writer.Write("data"u8.ToArray());
        writer.Write(dataBytes);
        writer.Write(new byte[dataBytes]);
    }

    internal static void WriteExtensiblePcm24Wav(string path, bool validGuid)
    {
        const short channels = 1;
        const short bits = 24;
        const short blockAlign = channels * (bits / 8);
        using var stream = File.Create(path);
        using var writer = new BinaryWriter(stream);
        writer.Write("RIFF"u8.ToArray());
        writer.Write(64);
        writer.Write("WAVE"u8.ToArray());
        writer.Write("fmt "u8.ToArray());
        writer.Write(40);
        writer.Write(unchecked((short)0xFFFE));
        writer.Write(channels);
        writer.Write(48_000);
        writer.Write(48_000 * blockAlign);
        writer.Write(blockAlign);
        writer.Write(bits);
        writer.Write((short)22);
        writer.Write(bits);
        writer.Write(0);
        writer.Write(1);
        writer.Write(validGuid
            ? new byte[] { 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 }
            : new byte[12]);
        writer.Write("data"u8.ToArray());
        writer.Write(3);
        writer.Write(new byte[3]);
        writer.Write((byte)0);
    }
}

internal sealed class RecordingCaptureNotificationService : ICaptureNotificationService
{
    public int PrepareCalls { get; private set; }
    public int NotifyCalls { get; private set; }
    public List<string> SavedFilenames { get; } = [];
    public List<(string Filename, string Reason)> FailedCaptures { get; } = [];
    public bool ThrowOnPrepare { get; set; }
    public bool ThrowOnNotify { get; set; }

    public void PrepareForCapture()
    {
        PrepareCalls++;
        if (ThrowOnPrepare)
        {
            throw new InvalidOperationException("Synthetic notification preparation failure.");
        }
    }

    public void NotifyCaptureSaved(string filename)
    {
        NotifyCalls++;
        if (ThrowOnNotify)
        {
            throw new InvalidOperationException("Synthetic notification delivery failure.");
        }
        SavedFilenames.Add(filename);
    }

    public void NotifyCaptureFailed(string filename, string reason)
    {
        NotifyCalls++;
        if (ThrowOnNotify)
        {
            throw new InvalidOperationException("Synthetic notification delivery failure.");
        }
        FailedCaptures.Add((filename, reason));
    }
}

internal sealed class FakeWorkerClient : ICaptureWorkerClient
{
    public string WorkerPath => "fake-worker.exe";
    public bool WorkerAvailable => true;
    public int TestCalls { get; private set; }
    public int CaptureCalls { get; private set; }
    public Exception? CaptureException { get; set; }
    public Exception? CaptureCancellationRaceException { get; set; }
    public bool HangTest { get; set; }
    public bool HoldTestCompletion { get; set; }
    public bool HoldCaptureCompletion { get; set; }
    public bool WriteCorruptCapture { get; set; }
    public bool CreateNativeSiblingTemporary { get; set; }
    public long CaptureFileSizeAdjustment { get; set; }
    public string? CaptureResultPath { get; set; }
    public short CaptureFileChannels { get; set; } = 1;
    public short CaptureFileBitDepth { get; set; } = 24;
    public int CaptureResultChannels { get; set; } = 1;
    public int CaptureResultBitDepth { get; set; } = 24;
    public IReadOnlyList<DiagnosticInfo> CaptureWarnings { get; set; } = [];
    public TaskCompletionSource CaptureOutputWritten { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
    public TaskCompletionSource ReleaseCaptureCompletion { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
    public TaskCompletionSource TestStarted { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
    public TaskCompletionSource ReleaseTestCompletion { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
    public SetupTestResult TestResult { get; set; } = new(
        Passed: true,
        SampleRate: 48_000,
        OutputPeakDbfs: -12,
        InputPeakDbfs: -18,
        LatencyFrames: 60,
        LatencyMilliseconds: 1.25,
        TimingErrorFrames: 4,
        Reliability: "reliable",
        Warnings: [],
        Failures: []);

    public Task<IReadOnlyList<AudioDeviceInfo>> GetDevicesAsync(CancellationToken cancellationToken)
        => Task.FromResult<IReadOnlyList<AudioDeviceInfo>>([
            new AudioDeviceInfo("asio:{FAKE-DRIVER}", "Fake ASIO", 2, 2, 48_000, true, "ASIO"),
        ]);

    public Task<DeviceChannels> GetChannelsAsync(string driverId, CancellationToken cancellationToken)
        => Task.FromResult(new DeviceChannels(
            driverId,
            "Fake ASIO",
            48_000,
            [new AudioChannelInfo(1, "Input 1"), new AudioChannelInfo(2, "Input 2")],
            [new AudioChannelInfo(1, "Output 1"), new AudioChannelInfo(2, "Output 2")]));

    public async Task<SetupTestResult> TestAsync(
        SetupTestRequest request,
        IProgress<WorkerProgress>? progress,
        CancellationToken cancellationToken)
    {
        TestCalls++;
        TestStarted.TrySetResult();
        if (HangTest)
        {
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
        }
        if (HoldTestCompletion)
        {
            await ReleaseTestCompletion.Task.WaitAsync(cancellationToken);
        }
        progress?.Report(new WorkerProgress("recording", 0.5, 1));
        progress?.Report(new WorkerProgress("verification", 1));
        return TestResult;
    }

    public async Task<CaptureCompleted> CaptureAsync(
        CaptureRequest request,
        IProgress<WorkerProgress>? progress,
        CancellationToken cancellationToken)
    {
        CaptureCalls++;
        if (CaptureCancellationRaceException is { } cancellationRaceException)
        {
            try
            {
                await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                throw cancellationRaceException;
            }
        }
        if (CaptureException is not null)
        {
            throw CaptureException;
        }
        progress?.Report(new WorkerProgress("recording", 0.5, 1));
        if (WriteCorruptCapture)
        {
            await File.WriteAllBytesAsync(request.OutputPath, [1, 2, 3], cancellationToken);
        }
        else
        {
            cancellationToken.ThrowIfCancellationRequested();
            Fixture.WritePcmWav(
                request.OutputPath,
                sampleRate: 48_000,
                frames: 480,
                channels: CaptureFileChannels,
                bitDepth: CaptureFileBitDepth);
        }
        if (CreateNativeSiblingTemporary)
        {
            await File.WriteAllBytesAsync(
                request.OutputPath + ".capture-panel.tmp.999.0",
                [1, 2, 3],
                CancellationToken.None);
        }
        CaptureOutputWritten.TrySetResult();
        if (HoldCaptureCompletion)
        {
            await ReleaseCaptureCompletion.Task;
        }
        progress?.Report(new WorkerProgress("complete", 1));
        var fileSize = new FileInfo(request.OutputPath).Length;
        return new CaptureCompleted(
            CaptureResultPath ?? request.OutputPath,
            fileSize + CaptureFileSizeAdjustment,
            CaptureResultChannels,
            CaptureResultBitDepth,
            48_000,
            0.1,
            60,
            1.25,
            480,
            480,
            CaptureWarnings);
    }
}

internal sealed class RacingChannelWorkerClient : ICaptureWorkerClient
{
    public string WorkerPath => "racing-worker.exe";
    public bool WorkerAvailable => true;
    public TaskCompletionSource DeviceBRequested { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
    public TaskCompletionSource ReleaseDeviceB { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);

    public Task<IReadOnlyList<AudioDeviceInfo>> GetDevicesAsync(CancellationToken cancellationToken)
        => Task.FromResult<IReadOnlyList<AudioDeviceInfo>>([
            new AudioDeviceInfo("asio:A", "Device A", 1, 1, 48_000, true, "ASIO"),
            new AudioDeviceInfo("asio:B", "Device B", 1, 1, 48_000, true, "ASIO"),
        ]);

    public async Task<DeviceChannels> GetChannelsAsync(string driverId, CancellationToken cancellationToken)
    {
        if (driverId == "asio:B")
        {
            DeviceBRequested.TrySetResult();
            await ReleaseDeviceB.Task;
        }

        var prefix = driverId == "asio:A" ? "A" : "B";
        var channelIndex = driverId == "asio:A" ? 1 : 7;
        return new DeviceChannels(
            driverId,
            $"Device {prefix}",
            48_000,
            [new AudioChannelInfo(channelIndex, $"{prefix} Input")],
            [new AudioChannelInfo(channelIndex, $"{prefix} Output")]);
    }

    public Task<SetupTestResult> TestAsync(
        SetupTestRequest request,
        IProgress<WorkerProgress>? progress,
        CancellationToken cancellationToken)
        => throw new NotSupportedException();

    public Task<CaptureCompleted> CaptureAsync(
        CaptureRequest request,
        IProgress<WorkerProgress>? progress,
        CancellationToken cancellationToken)
        => throw new NotSupportedException();
}

internal sealed class TimeoutWorkerClient(bool timeoutDevices) : ICaptureWorkerClient
{
    public string WorkerPath => "timeout-worker.exe";
    public bool WorkerAvailable => true;

    public Task<IReadOnlyList<AudioDeviceInfo>> GetDevicesAsync(CancellationToken cancellationToken)
        => timeoutDevices
            ? Task.FromException<IReadOnlyList<AudioDeviceInfo>>(new OperationCanceledException())
            : Task.FromResult<IReadOnlyList<AudioDeviceInfo>>([
                new AudioDeviceInfo("asio:timeout", "Slow ASIO", 1, 1, 48_000, true, "ASIO"),
            ]);

    public Task<DeviceChannels> GetChannelsAsync(string driverId, CancellationToken cancellationToken)
        => Task.FromException<DeviceChannels>(new OperationCanceledException());

    public Task<SetupTestResult> TestAsync(
        SetupTestRequest request,
        IProgress<WorkerProgress>? progress,
        CancellationToken cancellationToken)
        => throw new NotSupportedException();

    public Task<CaptureCompleted> CaptureAsync(
        CaptureRequest request,
        IProgress<WorkerProgress>? progress,
        CancellationToken cancellationToken)
        => throw new NotSupportedException();
}

internal sealed class HangingDiscoveryWorkerClient : ICaptureWorkerClient
{
    public string WorkerPath => "hanging-discovery-worker.exe";
    public bool WorkerAvailable => true;
    public TaskCompletionSource Started { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
    public bool CancellationObserved { get; private set; }

    public async Task<IReadOnlyList<AudioDeviceInfo>> GetDevicesAsync(CancellationToken cancellationToken)
    {
        Started.TrySetResult();
        try
        {
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            throw new InvalidOperationException("The discovery wait unexpectedly completed.");
        }
        catch (OperationCanceledException)
        {
            CancellationObserved = true;
            throw;
        }
    }

    public Task<DeviceChannels> GetChannelsAsync(string driverId, CancellationToken cancellationToken)
        => throw new NotSupportedException();

    public Task<SetupTestResult> TestAsync(
        SetupTestRequest request,
        IProgress<WorkerProgress>? progress,
        CancellationToken cancellationToken)
        => throw new NotSupportedException();

    public Task<CaptureCompleted> CaptureAsync(
        CaptureRequest request,
        IProgress<WorkerProgress>? progress,
        CancellationToken cancellationToken)
        => throw new NotSupportedException();
}

internal sealed class FakeFileDialogService(string sourcePath, string? destinationPath) : IFileDialogService
{
    public string? ChooseSourceWav() => sourcePath;
    public string? ChooseCaptureDestination(string defaultFilename) => destinationPath;
}

internal sealed class ThrowingFileDialogService(
    string sourcePath,
    Exception destinationException) : IFileDialogService
{
    public string? ChooseSourceWav() => sourcePath;

    public string? ChooseCaptureDestination(string defaultFilename)
        => throw destinationException;
}

internal sealed class MemorySettingsStore : IAppSettingsStore
{
    private AppSettings _settings = new();
    public AppSettings Load() => _settings;
    public void Save(AppSettings settings) => _settings = settings;
}
