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
    private static readonly TimeSpan WorkerTerminationTimeout = TimeSpan.FromSeconds(3);
    private readonly SemaphoreSlim _workerGate = new(1, 1);

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
        var exitCode = await RunWorkerAsync(
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
            acceptNonZeroResult: candidate => candidate == 1).ConfigureAwait(false);
        var completed = result ?? throw ProtocolError("The worker did not return a setup-test result.");
        var expectedExitCode = completed.Passed ? 0 : 1;
        if (exitCode != expectedExitCode)
        {
            throw ProtocolError(
                $"The setup-test result requires exit code {expectedExitCode}, but the worker exited with {exitCode}.");
        }
        return completed;
    }

    public async Task<CaptureCompleted> CaptureAsync(
        CaptureRequest request,
        IProgress<WorkerProgress>? progress,
        CancellationToken cancellationToken)
    {
        CaptureCompleted? result = null;
        var warnings = new List<DiagnosticInfo>();
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
                    stage = ReportEvent(element, stage, progressReporter, warnings);
                }
                else if (type == "capture_result")
                {
                    result = ParseCaptureResult(element);
                }
            },
            cancellationToken).ConfigureAwait(false);
        return result is null
            ? throw ProtocolError("The worker did not return a capture result.")
            : result with { Warnings = warnings.ToArray() };
    }

    private async Task<int> RunWorkerAsync(
        IReadOnlyList<string> arguments,
        Action<JsonElement> onMessage,
        CancellationToken cancellationToken,
        Func<int, bool>? acceptNonZeroResult = null)
    {
        await _workerGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            return await RunWorkerProcessAsync(
                arguments,
                onMessage,
                cancellationToken,
                acceptNonZeroResult).ConfigureAwait(false);
        }
        finally
        {
            _workerGate.Release();
        }
    }

    private async Task<int> RunWorkerProcessAsync(
        IReadOnlyList<string> arguments,
        Action<JsonElement> onMessage,
        CancellationToken cancellationToken,
        Func<int, bool>? acceptNonZeroResult)
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
            WorkingDirectory = Path.GetDirectoryName(WorkerPath) ?? AppContext.BaseDirectory,
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
                $"Could not start the native audio worker: {exception.Message}",
                innerException: exception);
        }

        WorkerProcessJob job;
        try
        {
            job = WorkerProcessJob.Attach(process);
        }
        catch (Exception exception)
        {
            TryTerminate(process);
            _ = await WaitForExitBoundedAsync(process).ConfigureAwait(false);
            throw new CaptureWorkerException(
                "worker_isolation_failed",
                $"Could not isolate the native audio worker: {exception.Message}",
                innerException: exception);
        }

        using var workerJob = job;
        using var cancellationRegistration = cancellationToken.Register(() => TryTerminate(process));
        var standardErrorTask = process.StandardError.ReadToEndAsync(CancellationToken.None);
        CaptureWorkerException? workerError = null;
        try
        {
            while (await process.StandardOutput.ReadLineAsync(cancellationToken).ConfigureAwait(false) is { } line)
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

            await process.WaitForExitAsync(cancellationToken).ConfigureAwait(false);
            var standardError = (await standardErrorTask
                .WaitAsync(WorkerTerminationTimeout)
                .ConfigureAwait(false)).Trim();
            if (cancellationToken.IsCancellationRequested)
            {
                throw new OperationCanceledException(cancellationToken);
            }
            if (workerError is not null)
            {
                throw workerError;
            }
            if (process.ExitCode != 0 && !(acceptNonZeroResult?.Invoke(process.ExitCode) ?? false))
            {
                var detail = standardError.Length == 0
                    ? $"The audio worker exited with code {process.ExitCode}."
                    : standardError;
                throw new CaptureWorkerException("worker_failed", detail, process.ExitCode);
            }
            return process.ExitCode;
        }
        catch
        {
            TryTerminate(process);
            job.Terminate();
            _ = await WaitForExitBoundedAsync(process).ConfigureAwait(false);
            await ObserveStandardErrorCompletionAsync(standardErrorTask).ConfigureAwait(false);
            throw;
        }
    }

    private static string ReportEvent(
        JsonElement element,
        string currentStage,
        ThrottledProgressReporter progress,
        List<DiagnosticInfo>? warnings = null)
    {
        var eventName = GetString(element, "event") ?? string.Empty;
        var stage = GetString(element, "stage") ?? currentStage;
        if (eventName == "warning"
            && warnings is not null
            && element.TryGetProperty("warning", out var warning)
            && warning.ValueKind == JsonValueKind.Object)
        {
            var code = GetString(warning, "code") ?? "unknown";
            var message = GetString(element, "message")
                ?? GetString(warning, "message")
                ?? "The audio worker reported a capture warning.";
            if (!warnings.Any(item => string.Equals(item.Code, code, StringComparison.Ordinal)))
            {
                warnings.Add(new DiagnosticInfo(code, message));
            }
        }
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
        var alignment = GetRequiredObject(element, "alignment");
        var verification = GetRequiredObject(element, "verification");
        var sweep = verification.TryGetProperty("sweep", out var sweepElement)
            && sweepElement.ValueKind == JsonValueKind.Object
            ? sweepElement
            : default;
        var reliability = sweep.ValueKind == JsonValueKind.Object
            ? GetRequiredString(sweep, "reliability")
            : "unmeasurable";
        if (reliability is not ("reliable" or "ambiguous" or "unmeasurable"))
        {
            throw ProtocolError($"The worker returned an unknown verification reliability '{reliability}'.");
        }

        var passed = GetRequiredBoolean(element, "passed");
        var failures = ParseDiagnostics(element, "failures", required: true);
        if (passed != (failures.Count == 0))
        {
            throw ProtocolError("The setup-test pass flag disagrees with its failure diagnostics.");
        }

        return new SetupTestResult(
            passed,
            GetRequiredFiniteDouble(
                element,
                "sampleRate",
                CaptureLimits.MinimumSampleRate,
                CaptureLimits.MaximumSampleRate),
            GetRequiredFiniteDouble(element, "outputPeakDbfs", minimum: -1_000, maximum: 1_000),
            GetRequiredFiniteDouble(element, "inputPeakDbfs", minimum: -1_000, maximum: 1_000),
            GetNullableInt64Strict(alignment, "markerLatencyFrames"),
            GetNullableFiniteDouble(alignment, "markerLatencyMilliseconds", -3_600_000, 3_600_000),
            GetNullableFiniteDouble(verification, "timingFitErrorFrames", 0, long.MaxValue),
            sweep.ValueKind == JsonValueKind.Object
                ? reliability
                : "unmeasurable",
            ParseDiagnostics(element, "warnings", required: true),
            failures);
    }

    private static CaptureCompleted ParseCaptureResult(JsonElement element)
    {
        var output = GetRequiredObject(element, "output");
        var alignment = GetRequiredObject(element, "alignment");
        var outputPath = GetRequiredString(output, "path");
        if (string.IsNullOrWhiteSpace(outputPath))
        {
            throw ProtocolError("The worker returned an empty capture output path.");
        }

        var channelCount = GetRequiredInt32(output, "channelCount", 1, 1024);
        var bitDepth = GetRequiredInt32(output, "bitDepth", 1, 64);
        if (bitDepth is not (16 or 24 or 32))
        {
            throw ProtocolError($"The worker returned an unsupported output bit depth: {bitDepth}.");
        }
        var trimmedFrames = GetRequiredInt64(alignment, "trimmedFrameCount", 0, long.MaxValue);
        var targetFrames = GetRequiredInt64(alignment, "targetFrameCount", 1, long.MaxValue);
        if (trimmedFrames > targetFrames)
        {
            throw ProtocolError("The worker returned more trimmed frames than target frames.");
        }

        return new CaptureCompleted(
            outputPath,
            GetRequiredInt64(output, "fileSize", 1, long.MaxValue),
            channelCount,
            bitDepth,
            GetRequiredFiniteDouble(
                output,
                "sampleRate",
                CaptureLimits.MinimumSampleRate,
                CaptureLimits.MaximumSampleRate),
            GetRequiredFiniteDouble(element, "elapsedSeconds", 0, 7 * 24 * 60 * 60),
            GetNullableInt64Strict(alignment, "markerLatencyFrames"),
            GetNullableFiniteDouble(alignment, "markerLatencyMilliseconds", -3_600_000, 3_600_000),
            trimmedFrames,
            targetFrames,
            []);
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

    private static IReadOnlyList<DiagnosticInfo> ParseDiagnostics(
        JsonElement element,
        string propertyName,
        bool required = false)
    {
        if (!element.TryGetProperty(propertyName, out var diagnostics)
            || diagnostics.ValueKind != JsonValueKind.Array)
        {
            if (required)
            {
                throw ProtocolError($"A worker message has no '{propertyName}' array.");
            }
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
        => element.TryGetProperty(propertyName, out var property)
            && property.TryGetDouble(out var value)
            && double.IsFinite(value)
            ? value
            : 0;

    private static double? GetNullableDouble(JsonElement element, string propertyName)
        => element.TryGetProperty(propertyName, out var property)
            && property.ValueKind == JsonValueKind.Number
            && property.TryGetDouble(out var value)
            && double.IsFinite(value)
            ? value
            : null;

    private static long? GetNullableInt64(JsonElement element, string propertyName)
        => element.TryGetProperty(propertyName, out var property)
            && property.ValueKind == JsonValueKind.Number
            && property.TryGetInt64(out var value)
            ? value
            : null;

    private static JsonElement GetRequiredObject(JsonElement element, string propertyName)
        => element.TryGetProperty(propertyName, out var property)
            && property.ValueKind == JsonValueKind.Object
            ? property
            : throw ProtocolError($"A worker message has no '{propertyName}' object.");

    private static string GetRequiredString(JsonElement element, string propertyName)
        => GetString(element, propertyName)
            ?? throw ProtocolError($"A worker message has no string '{propertyName}' field.");

    private static bool GetRequiredBoolean(JsonElement element, string propertyName)
        => element.TryGetProperty(propertyName, out var property)
            && property.ValueKind is JsonValueKind.True or JsonValueKind.False
            ? property.GetBoolean()
            : throw ProtocolError($"A worker message has no boolean '{propertyName}' field.");

    private static double GetRequiredFiniteDouble(
        JsonElement element,
        string propertyName,
        double minimum,
        double maximum)
    {
        if (!element.TryGetProperty(propertyName, out var property)
            || !property.TryGetDouble(out var value)
            || !double.IsFinite(value)
            || value < minimum
            || value > maximum)
        {
            throw ProtocolError($"The worker returned an invalid '{propertyName}' value.");
        }
        return value;
    }

    private static double? GetNullableFiniteDouble(
        JsonElement element,
        string propertyName,
        double minimum,
        double maximum)
    {
        if (!element.TryGetProperty(propertyName, out var property)
            || property.ValueKind == JsonValueKind.Null)
        {
            return null;
        }
        if (!property.TryGetDouble(out var value)
            || !double.IsFinite(value)
            || value < minimum
            || value > maximum)
        {
            throw ProtocolError($"The worker returned an invalid '{propertyName}' value.");
        }
        return value;
    }

    private static int GetRequiredInt32(
        JsonElement element,
        string propertyName,
        int minimum,
        int maximum)
    {
        if (!element.TryGetProperty(propertyName, out var property)
            || !property.TryGetInt32(out var value)
            || value < minimum
            || value > maximum)
        {
            throw ProtocolError($"The worker returned an invalid '{propertyName}' value.");
        }
        return value;
    }

    private static long GetRequiredInt64(
        JsonElement element,
        string propertyName,
        long minimum,
        long maximum)
    {
        if (!element.TryGetProperty(propertyName, out var property)
            || !property.TryGetInt64(out var value)
            || value < minimum
            || value > maximum)
        {
            throw ProtocolError($"The worker returned an invalid '{propertyName}' value.");
        }
        return value;
    }

    private static long? GetNullableInt64Strict(JsonElement element, string propertyName)
    {
        if (!element.TryGetProperty(propertyName, out var property)
            || property.ValueKind == JsonValueKind.Null)
        {
            return null;
        }
        return property.TryGetInt64(out var value)
            ? value
            : throw ProtocolError($"The worker returned an invalid '{propertyName}' value.");
    }

    private static CaptureWorkerException ProtocolError(string message)
        => new("protocol_error", message);

    private static async Task<bool> WaitForExitBoundedAsync(Process process)
    {
        try
        {
            if (process.HasExited)
            {
                return true;
            }
            using var timeout = new CancellationTokenSource(WorkerTerminationTimeout);
            await process.WaitForExitAsync(timeout.Token).ConfigureAwait(false);
            return true;
        }
        catch (InvalidOperationException)
        {
            return true;
        }
        catch (OperationCanceledException)
        {
            return false;
        }
    }

    private static async Task ObserveStandardErrorCompletionAsync(Task<string> standardErrorTask)
    {
        try
        {
            _ = await standardErrorTask.WaitAsync(WorkerTerminationTimeout).ConfigureAwait(false);
        }
        catch (Exception)
        {
        }
    }

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
