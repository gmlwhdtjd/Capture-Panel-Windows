namespace CapturePanel.App.Models;

public sealed record WavMetadata(
    int SampleRate,
    int Channels,
    int BitsPerSample,
    long Frames,
    double DurationSeconds);

public readonly record struct FileIdentity(uint VolumeSerialNumber, ulong FileIndex);

public sealed record WavFileSnapshot(
    string FullPath,
    WavMetadata Metadata,
    long FileLength,
    DateTime LastWriteTimeUtc,
    FileIdentity? Identity,
    string ContentSha256);
