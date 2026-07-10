namespace CapturePanel.App.Models;

public sealed record DiagnosticInfo(string Code, string Message);

public sealed record WorkerProgress(
    string Stage,
    double? Fraction = null,
    double? RemainingSeconds = null,
    string? Message = null);

public sealed record SetupTestRequest(
    string DriverId,
    int PlaybackChannel,
    int RecordChannel,
    double OutputTrimDb,
    double InputTrimDb,
    double SampleRate);

public sealed record SetupTestResult(
    bool Passed,
    double SampleRate,
    double OutputPeakDbfs,
    double InputPeakDbfs,
    long? LatencyFrames,
    double? LatencyMilliseconds,
    double? TimingErrorFrames,
    string Reliability,
    IReadOnlyList<DiagnosticInfo> Warnings,
    IReadOnlyList<DiagnosticInfo> Failures);

public sealed record CaptureRequest(
    string InputPath,
    string OutputPath,
    string DriverId,
    int PlaybackChannel,
    int RecordChannel,
    double OutputTrimDb,
    double InputTrimDb);

public sealed record CaptureCompleted(
    string OutputPath,
    long FileSize,
    int Channels,
    int BitDepth,
    double SampleRate,
    double ElapsedSeconds,
    long? LatencyFrames,
    double? LatencyMilliseconds,
    long TrimmedFrames,
    long TargetFrames);

public enum CaptureStabilityLevel
{
    Unknown,
    Excellent,
    Good,
    Caution,
    Unstable,
    Failed,
}

public enum MeterState
{
    Empty,
    Normal,
    Hot,
    Clipping,
}

public enum SetupInputLevel
{
    Unavailable,
    Low,
    Normal,
    Hot,
    Clipping,
}

public enum StatusKind
{
    Neutral,
    Working,
    Ready,
    Warning,
    Error,
}
