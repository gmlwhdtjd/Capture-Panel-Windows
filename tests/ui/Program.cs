using System.IO;
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
    private static async Task<int> Main()
    {
        var tests = new (string Name, Func<Task> Run)[]
        {
            ("output device selection synchronizes the disabled input device", DeviceSelectionSynchronizesInput),
            ("source test capture gate and trim invalidation", SourceTestGateAndTrimInvalidation),
            ("setup input level uses the Mac verification boundaries", SetupInputLevelUsesMacBoundaries),
            ("clipping blocks capture and reports failed stability", ClippingBlocksCapture),
            ("capture uses a temporary output and promotes it on success", CapturePromotesTemporaryOutput),
            ("late worker completion cannot win a capture cancellation", LateCaptureCompletionStaysCancelled),
            ("capture failure invalidates setup and remains visible", CaptureFailureInvalidatesSetup),
            ("stale channel discovery cannot overwrite a newer device", StaleChannelDiscoveryIsIgnored),
            ("fallback channels are persisted after a driver change", FallbackChannelsArePersisted),
            ("device discovery timeout becomes a recoverable UI error", DeviceDiscoveryTimeoutIsRecoverable),
            ("channel discovery timeout invalidates the route", ChannelDiscoveryTimeoutInvalidatesRoute),
            ("setup test watchdog unlocks a hung worker", SetupTestWatchdogUnlocksUi),
            ("disposing during discovery cancels the worker", DisposeCancelsDeviceDiscovery),
            ("JSON settings persist the Windows route", JsonSettingsRoundTrip),
            ("WAV metadata reader reports source format and frames", WavMetadataRoundTrip),
            ("native JSON worker contract supports Fake test and capture", NativeWorkerJsonContract),
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
    }

    private static async Task LateCaptureCompletionStaysCancelled()
    {
        using var fixture = new Fixture();
        fixture.Worker.HoldCaptureCompletion = true;
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

        Require(!File.Exists(fixture.DestinationPath),
            "A cancelled capture must never promote a late temporary result.");
        Equal("Capture cancelled.", model.Assessment);
        Require(model.CanCapture, "Cancellation should preserve the previously verified route.");
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

    private static Task WavMetadataRoundTrip()
    {
        using var fixture = new Fixture();
        var metadata = WavMetadataReader.Read(fixture.SourcePath);
        Equal(48_000, metadata.SampleRate);
        Equal(1, metadata.Channels);
        Equal(24, metadata.BitsPerSample);
        Equal(480L, metadata.Frames);
        return Task.CompletedTask;
    }

    private static async Task NativeWorkerJsonContract()
    {
        using var fixture = new Fixture();
        var worker = new CaptureWorkerClient(Path.Combine(AppContext.BaseDirectory, "capture-panel.exe"));
        Require(worker.WorkerAvailable, "The native worker was not copied beside the managed tests.");

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

        var outputPath = Path.Combine(fixture.DirectoryPath, "native-capture.wav");
        var captureResult = await worker.CaptureAsync(
            new CaptureRequest(fixture.SourcePath, outputPath, "fake:loopback", 1, 1, 0, 0),
            progress: null,
            timeout.Token);
        Require(File.Exists(outputPath), "The native Fake capture did not write an output WAV.");
        Equal(Path.GetFullPath(outputPath), Path.GetFullPath(captureResult.OutputPath));
        Equal(480L, WavMetadataReader.Read(outputPath).Frames);
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
}

internal sealed class Fixture : IDisposable
{
    public Fixture()
    {
        DirectoryPath = Path.Combine(Path.GetTempPath(), "capture-panel-ui-tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(DirectoryPath);
        SourcePath = Path.Combine(DirectoryPath, "source.wav");
        DestinationPath = Path.Combine(DirectoryPath, "source-captured.wav");
        WritePcm24Wav(SourcePath, sampleRate: 48_000, frames: 480);
        Worker = new FakeWorkerClient();
        Dialogs = new FakeFileDialogService(SourcePath, DestinationPath);
        Settings = new MemorySettingsStore();
    }

    public string DirectoryPath { get; }
    public string SourcePath { get; }
    public string DestinationPath { get; }
    public FakeWorkerClient Worker { get; }
    public FakeFileDialogService Dialogs { get; }
    public MemorySettingsStore Settings { get; }

    public MainViewModel CreateModel()
        => new(Worker, Settings, Dialogs, showDevelopmentDevices: true);

    public void Dispose()
    {
        if (Directory.Exists(DirectoryPath))
        {
            Directory.Delete(DirectoryPath, recursive: true);
        }
    }

    private static void WritePcm24Wav(string path, int sampleRate, int frames)
    {
        const short channels = 1;
        const short bits = 24;
        const short blockAlign = channels * (bits / 8);
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
        writer.Write(bits);
        writer.Write("data"u8.ToArray());
        writer.Write(dataBytes);
        writer.Write(new byte[dataBytes]);
    }
}

internal sealed class FakeWorkerClient : ICaptureWorkerClient
{
    public string WorkerPath => "fake-worker.exe";
    public bool WorkerAvailable => true;
    public int TestCalls { get; private set; }
    public int CaptureCalls { get; private set; }
    public Exception? CaptureException { get; set; }
    public bool HangTest { get; set; }
    public bool HoldCaptureCompletion { get; set; }
    public TaskCompletionSource CaptureOutputWritten { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
    public TaskCompletionSource ReleaseCaptureCompletion { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
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
        if (HangTest)
        {
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
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
        if (CaptureException is not null)
        {
            throw CaptureException;
        }
        progress?.Report(new WorkerProgress("recording", 0.5, 1));
        await File.WriteAllBytesAsync(request.OutputPath, [1, 2, 3], cancellationToken);
        CaptureOutputWritten.TrySetResult();
        if (HoldCaptureCompletion)
        {
            await ReleaseCaptureCompletion.Task;
        }
        progress?.Report(new WorkerProgress("complete", 1));
        return new CaptureCompleted(
            request.OutputPath,
            3,
            1,
            24,
            48_000,
            0.1,
            60,
            1.25,
            480,
            480);
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

internal sealed class FakeFileDialogService(string sourcePath, string destinationPath) : IFileDialogService
{
    public string? ChooseSourceWav() => sourcePath;
    public string? ChooseCaptureDestination(string defaultFilename) => destinationPath;
}

internal sealed class MemorySettingsStore : IAppSettingsStore
{
    private AppSettings _settings = new();
    public AppSettings Load() => _settings;
    public void Save(AppSettings settings) => _settings = settings;
}
