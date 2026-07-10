namespace CapturePanel.App.Models;

public sealed record WavMetadata(
    int SampleRate,
    int Channels,
    int BitsPerSample,
    long Frames,
    double DurationSeconds);
