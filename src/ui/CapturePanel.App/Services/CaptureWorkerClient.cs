using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.Json;
using CapturePanel.App.Models;

namespace CapturePanel.App.Services;

public sealed class CaptureWorkerClient : ICaptureWorkerClient
{
    private const string Protocol = "capture-panel/1";

    public CaptureWorkerClient(string workerPath)
    {
        WorkerPath = Path.GetFullPath(workerPath);
    }

    public string WorkerPath { get; }
    public bool WorkerAvailable => File.Exists(WorkerPath);

    public async Task<IReadOnlyList<AudioDeviceInfo>> GetDevicesAsync(CancellationToken cancellationToken)
    {
        IReadOnlyList<AudioDeviceInfo>? result = null;
        await RunWorkerAsync(
            ["devices", "--json"],
            element =>
            {
                if (TypeOf(element) == "devices")
                {
                    result = element.GetProperty("devices").EnumerateArray().Select(ParseDevice).ToArray();
                }
            },
            cancellationToken).ConfigureAwait(false);
        return result ?? throw ProtocolError("The worker did not return a devices result.");
    }

    public async Task<DeviceChannels> GetChannelsAsync(string driverId, CancellationToken cancellationToken)
    {
        DeviceChannels? result = null;
        await RunWorkerAsync(
            ["channels", "--driver", driverId, "--json"],
            element =>
            {
                if (TypeOf(element) != "channels")
                {
                    return;
                }

                var device = ParseDevice(element.GetProperty("device"));
                result = new DeviceChannels(
                    device.Id,
                    device.Name,
                    device.SampleRate,
                    ParseChannels(element.GetProperty("inputs")),
                    ParseChannels(element.GetProperty("outputs")));
            },
            cancellationToken).ConfigureAwait(false);
        return result ?? throw ProtocolError("The worker did not return a channels result.");
    }

    public async Task<SetupTestResult> TestAsync(
        SetupTestRequest request,
        IProgress<WorkerProgress>? progress,
        CancellationToken cancellationToken)
    {
        SetupTestResult? result = null;
        var stage = "sample_rate_configuration";
        var progressReporter = new ThrottledProgressReporter(progress);
        await RunWorkerAsync(
            [
                "test",
                "--driver", request.DriverId,
                "--play-channel", request.PlaybackChannel.ToString(CultureInfo.InvariantCulture),
                "--record-channel", request.RecordChannel.ToString(CultureInfo.InvariantCulture),
                "--output-trim", request.OutputTrimDb.ToString("0.########", CultureInfo.InvariantCulture),
                "--input-trim", request.InputTrimDb.ToString("0.########", CultureInfo.InvariantCulture),
                "--sample-rate", request.SampleRate.ToString("0.########", CultureInfo.InvariantCulture),
                "--verbose",
                "--json",
            ],
            element =>
            {
                var type = TypeOf(element);
                if (type == "event")
                {
                    stage = ReportEvent(element, stage, progressReporter);
                }
                else if (type == "test_result")
                {
                    result = ParseTestResult(element);
                }
            },
            cancellationToken,
            acceptNonZeroResult: () => result is not null).ConfigureAwait(false);
        return result ?? throw ProtocolError("The worker did not return a setup-test result.");
    }

    public async Task<CaptureCompleted> CaptureAsync(
        CaptureRequest request,
        IProgress<WorkerProgress>? progress,
        CancellationToken cancellationToken)
    {
        CaptureCompleted? result = null;
        var stage = "sample_rate_configuration";
        var progressReporter = new ThrottledProgressReporter(progress);
        await RunWorkerAsync(
            [
                "run",
                "--input", request.InputPath,
                "--output", request.OutputPath,
                "--driver", request.DriverId,
                "--play-channel", request.PlaybackChannel.ToString(CultureInfo.InvariantCulture),
                "--record-channel", request.RecordChannel.ToString(CultureInfo.InvariantCulture),
                "--output-trim", request.OutputTrimDb.ToString("0.########", CultureInfo.InvariantCulture),
                "--input-trim", request.InputTrimDb.ToString("0.########", CultureInfo.InvariantCulture),
                "--verbose",
                "--json",
            ],
            element =>
            {
                var type = TypeOf(element);
                if (type == "event")
                {
                    stage = ReportEvent(element, stage, progressReporter);
                }
                else if (type == "capture_result")
                {
                    result = ParseCaptureResult(element);
                }
            },
            cancellationToken).ConfigureAwait(false);
        return result ?? throw ProtocolError("The worker did not return a capture result.");
    }

    private async Task RunWorkerAsync(
        IReadOnlyList<string> arguments,
        Action<JsonElement> onMessage,
        CancellationToken cancellationToken,
        Func<bool>? acceptNonZeroResult = null)
    {
        if (!WorkerAvailable)
        {
            throw new CaptureWorkerException(
                "worker_missing",
                $"The native audio worker was not found: {WorkerPath}");
        }

        var startInfo = new ProcessStartInfo
        {
            FileName = WorkerPath,
            WorkingDirectory = AppContext.BaseDirectory,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = new UTF8Encoding(encoderShouldEmitUTF8Identifier: false),
            StandardErrorEncoding = new UTF8Encoding(encoderShouldEmitUTF8Identifier: false),
        };
        foreach (var argument in arguments)
        {
            startInfo.ArgumentList.Add(argument);
        }

        using var process = new Process { StartInfo = startInfo };
        try
        {
            if (!process.Start())
            {
                throw new CaptureWorkerException("worker_start_failed", "The native audio worker did not start.");
            }
        }
        catch (CaptureWorkerException)
        {
            throw;
        }
        catch (Exception exception)
        {
            throw new CaptureWorkerException(
                "worker_start_failed",
                $"Could not start the native audio worker: {exception.Message}");
        }

        using var cancellationRegistration = cancellationToken.Register(() => TryTerminate(process));
        var standardErrorTask = process.StandardError.ReadToEndAsync(CancellationToken.None);
        CaptureWorkerException? workerError = null;
        try
        {
            while (await process.StandardOutput.ReadLineAsync(CancellationToken.None).ConfigureAwait(false) is { } line)
            {
                if (line.Length == 0)
                {
                    continue;
                }

                JsonDocument document;
                try
                {
                    document = JsonDocument.Parse(line);
                }
                catch (JsonException exception)
                {
                    throw ProtocolError($"The worker emitted malformed JSON: {exception.Message}");
                }

                using (document)
                {
                    var root = document.RootElement;
                    if (!root.TryGetProperty("protocol", out var protocol)
                        || protocol.GetString() != Protocol)
                    {
                        throw ProtocolError("The native worker uses an incompatible protocol version.");
                    }

                    if (TypeOf(root) == "error")
                    {
                        workerError = ParseError(root);
                    }
                    else
                    {
                        onMessage(root);
                    }
                }
            }

            await process.WaitForExitAsync(CancellationToken.None).ConfigureAwait(false);
            var standardError = (await standardErrorTask.ConfigureAwait(false)).Trim();
            if (cancellationToken.IsCancellationRequested)
            {
                throw new OperationCanceledException(cancellationToken);
            }
            if (workerError is not null)
            {
                throw workerError;
            }
            if (process.ExitCode != 0 && !(acceptNonZeroResult?.Invoke() ?? false))
            {
                var detail = standardError.Length == 0
                    ? $"The audio worker exited with code {process.ExitCode}."
                    : standardError;
                throw new CaptureWorkerException("worker_failed", detail, process.ExitCode);
            }
        }
        catch
        {
            TryTerminate(process);
            try
            {
                await process.WaitForExitAsync(CancellationToken.None).ConfigureAwait(false);
            }
            catch (InvalidOperationException)
            {
            }
            _ = await standardErrorTask.ConfigureAwait(false);
            throw;
        }
    }

    private static string ReportEvent(
        JsonElement element,
        string currentStage,
        ThrottledProgressReporter progress)
    {
        var eventName = GetString(element, "event") ?? string.Empty;
        var stage = GetString(element, "stage") ?? currentStage;
        if (eventName == "stage_changed")
        {
            progress.Report(
                new WorkerProgress(stage, Message: GetString(element, "message")),
                force: true);
            return stage;
        }

        if (element.TryGetProperty("progress", out var progressElement))
        {
            var completed = GetDouble(progressElement, "completedFrames");
            var total = GetDouble(progressElement, "totalFrames");
            var fraction = total > 0
                ? completed / total
                : GetDouble(progressElement, "percentage") / 100.0;
            var workerProgress = new WorkerProgress(
                stage,
                Math.Clamp(fraction, 0, 1),
                GetNullableDouble(progressElement, "remainingSeconds"),
                GetString(element, "message"));
            progress.Report(workerProgress, force: fraction >= 1);
        }
        else if (eventName == "alignment_finished")
        {
            progress.Report(new WorkerProgress("alignment"), force: true);
        }
        return stage;
    }

    private sealed class ThrottledProgressReporter(IProgress<WorkerProgress>? progress)
    {
        private static readonly TimeSpan MinimumInterval = TimeSpan.FromMilliseconds(100);
        private readonly Stopwatch _stopwatch = Stopwatch.StartNew();

        public void Report(WorkerProgress value, bool force = false)
        {
            if (progress is null || (!force && _stopwatch.Elapsed < MinimumInterval))
            {
                return;
            }

            _stopwatch.Restart();
            progress.Report(value);
        }
    }

    private static SetupTestResult ParseTestResult(JsonElement element)
    {
        var alignment = element.GetProperty("alignment");
        var verification = element.GetProperty("verification");
        var sweep = verification.TryGetProperty("sweep", out var sweepElement)
            && sweepElement.ValueKind == JsonValueKind.Object
            ? sweepElement
            : default;
        return new SetupTestResult(
            element.GetProperty("passed").GetBoolean(),
            GetDouble(element, "sampleRate"),
            GetDouble(element, "outputPeakDbfs"),
            GetDouble(element, "inputPeakDbfs"),
            GetNullableInt64(alignment, "markerLatencyFrames"),
            GetNullableDouble(alignment, "markerLatencyMilliseconds"),
            GetNullableDouble(verification, "timingFitErrorFrames"),
            sweep.ValueKind == JsonValueKind.Object
                ? GetString(sweep, "reliability") ?? "unmeasurable"
                : "unmeasurable",
            ParseDiagnostics(element, "warnings"),
            ParseDiagnostics(element, "failures"));
    }

    private static CaptureCompleted ParseCaptureResult(JsonElement element)
    {
        var output = element.GetProperty("output");
        var alignment = element.GetProperty("alignment");
        return new CaptureCompleted(
            output.GetProperty("path").GetString() ?? string.Empty,
            output.GetProperty("fileSize").GetInt64(),
            output.GetProperty("channelCount").GetInt32(),
            output.GetProperty("bitDepth").GetInt32(),
            GetDouble(output, "sampleRate"),
            GetDouble(element, "elapsedSeconds"),
            GetNullableInt64(alignment, "markerLatencyFrames"),
            GetNullableDouble(alignment, "markerLatencyMilliseconds"),
            alignment.GetProperty("trimmedFrameCount").GetInt64(),
            alignment.GetProperty("targetFrameCount").GetInt64());
    }

    private static CaptureWorkerException ParseError(JsonElement element)
    {
        var code = GetString(element, "code") ?? GetString(element, "category") ?? "worker_error";
        var message = GetString(element, "message") ?? "The native audio worker reported an error.";
        return new CaptureWorkerException(code, message);
    }

    private static AudioDeviceInfo ParseDevice(JsonElement element)
        => new(
            element.GetProperty("id").GetString() ?? string.Empty,
            element.GetProperty("name").GetString() ?? "Unnamed ASIO driver",
            element.GetProperty("inputChannels").GetInt32(),
            element.GetProperty("outputChannels").GetInt32(),
            GetDouble(element, "sampleRate"),
            element.GetProperty("available").GetBoolean(),
            GetString(element, "status") ?? string.Empty);

    private static IReadOnlyList<AudioChannelInfo> ParseChannels(JsonElement element)
        => element.EnumerateArray()
            .Select(channel => new AudioChannelInfo(
                channel.GetProperty("index").GetInt32(),
                channel.GetProperty("name").GetString() ?? string.Empty))
            .ToArray();

    private static IReadOnlyList<DiagnosticInfo> ParseDiagnostics(JsonElement element, string propertyName)
    {
        if (!element.TryGetProperty(propertyName, out var diagnostics)
            || diagnostics.ValueKind != JsonValueKind.Array)
        {
            return [];
        }
        return diagnostics.EnumerateArray()
            .Select(item => new DiagnosticInfo(
                GetString(item, "code") ?? "unknown",
                GetString(item, "message") ?? string.Empty))
            .ToArray();
    }

    private static string TypeOf(JsonElement element)
        => GetString(element, "type") ?? throw ProtocolError("A worker message has no type.");

    private static string? GetString(JsonElement element, string propertyName)
        => element.TryGetProperty(propertyName, out var property)
            && property.ValueKind == JsonValueKind.String
            ? property.GetString()
            : null;

    private static double GetDouble(JsonElement element, string propertyName)
        => element.TryGetProperty(propertyName, out var property) && property.TryGetDouble(out var value)
            ? value
            : 0;

    private static double? GetNullableDouble(JsonElement element, string propertyName)
        => element.TryGetProperty(propertyName, out var property)
            && property.ValueKind == JsonValueKind.Number
            && property.TryGetDouble(out var value)
            ? value
            : null;

    private static long? GetNullableInt64(JsonElement element, string propertyName)
        => element.TryGetProperty(propertyName, out var property)
            && property.ValueKind == JsonValueKind.Number
            && property.TryGetInt64(out var value)
            ? value
            : null;

    private static CaptureWorkerException ProtocolError(string message)
        => new("protocol_error", message);

    private static void TryTerminate(Process process)
    {
        try
        {
            if (!process.HasExited)
            {
                process.Kill(entireProcessTree: true);
            }
        }
        catch (InvalidOperationException)
        {
        }
        catch (System.ComponentModel.Win32Exception)
        {
        }
    }
}
