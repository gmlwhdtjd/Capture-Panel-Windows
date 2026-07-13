using CapturePanel.App.Models;

namespace CapturePanel.App.Services;

public interface ICaptureWorkerClient
{
    string WorkerPath { get; }
    bool WorkerAvailable { get; }

    Task<IReadOnlyList<AudioDeviceInfo>> GetDevicesAsync(CancellationToken cancellationToken);

    Task<DeviceChannels> GetChannelsAsync(string driverId, CancellationToken cancellationToken);

    Task<SetupTestResult> TestAsync(
        SetupTestRequest request,
        IProgress<WorkerProgress>? progress,
        CancellationToken cancellationToken);

    Task<CaptureCompleted> CaptureAsync(
        CaptureRequest request,
        IProgress<WorkerProgress>? progress,
        CancellationToken cancellationToken);
}

public sealed class CaptureWorkerException(
    string code,
    string message,
    int? exitCode = null,
    Exception? innerException = null)
    : Exception(message, innerException)
{
    public string Code { get; } = code;
    public int? ExitCode { get; } = exitCode;
}
