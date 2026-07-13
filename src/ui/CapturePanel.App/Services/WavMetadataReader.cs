using System.Buffers.Binary;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using CapturePanel.App.Models;
using Microsoft.Win32.SafeHandles;

namespace CapturePanel.App.Services;

public static class WavMetadataReader
{
    private const ushort PcmFormat = 0x0001;
    private const ushort IeeeFloatFormat = 0x0003;
    private const ushort ExtensibleFormat = 0xFFFE;

    private static readonly byte[] CanonicalSubformatGuidTail =
    [
        0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
        0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71,
    ];

    public static WavMetadata Read(string path)
    {
        using var stream = OpenRead(path);
        return ReadMetadata(stream);
    }

    public static WavFileSnapshot ReadSnapshot(string path)
    {
        var fullPath = Path.GetFullPath(path);
        using var stream = OpenRead(fullPath);
        var metadata = ReadMetadata(stream);
        ValidateSourceMetadata(metadata);
        stream.Position = 0;
        var contentSha256 = Convert.ToHexString(SHA256.HashData(stream));
        return CreateSnapshot(fullPath, stream, metadata, contentSha256);
    }

    public static WavFileSnapshot ReadMetadataSnapshot(string path)
    {
        var fullPath = Path.GetFullPath(path);
        using var stream = OpenRead(fullPath);
        var metadata = ReadMetadata(stream);
        ValidateSourceMetadata(metadata);
        return CreateSnapshot(fullPath, stream, metadata, contentSha256: string.Empty);
    }

    public static async Task<WavFileSnapshot> ReadSnapshotAsync(
        string path,
        CancellationToken cancellationToken)
    {
        var fullPath = Path.GetFullPath(path);
        await using var stream = OpenRead(fullPath, asynchronous: true);
        var metadata = ReadMetadata(stream);
        ValidateSourceMetadata(metadata);
        stream.Position = 0;
        var contentSha256 = Convert.ToHexString(
            await SHA256.HashDataAsync(stream, cancellationToken).ConfigureAwait(false));
        cancellationToken.ThrowIfCancellationRequested();
        return CreateSnapshot(fullPath, stream, metadata, contentSha256);
    }

    private static WavFileSnapshot CreateSnapshot(
        string fullPath,
        FileStream stream,
        WavMetadata metadata,
        string contentSha256)
        => new(
            fullPath,
            metadata,
            stream.Length,
            File.GetLastWriteTimeUtc(fullPath),
            TryGetIdentity(stream.SafeFileHandle),
            contentSha256);

    private static void ValidateSourceMetadata(WavMetadata metadata)
    {
        if (metadata.SampleRate < CaptureLimits.MinimumSampleRate
            || metadata.SampleRate > CaptureLimits.MaximumSampleRate)
        {
            throw new InvalidDataException(
                $"The WAV sample rate must be between {CaptureLimits.MinimumSampleRate:0} and "
                + $"{CaptureLimits.MaximumSampleRate:0} Hz.");
        }
    }

    public static bool RefersToSameFile(WavFileSnapshot source, string candidatePath)
    {
        var fullCandidate = Path.GetFullPath(candidatePath);
        if (string.Equals(source.FullPath, fullCandidate, StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }
        if (source.Identity is null || !File.Exists(fullCandidate))
        {
            return false;
        }

        using var handle = File.OpenHandle(
            fullCandidate,
            FileMode.Open,
            FileAccess.Read,
            FileShare.ReadWrite | FileShare.Delete);
        var candidateIdentity = TryGetIdentity(handle);
        return candidateIdentity is not null && candidateIdentity == source.Identity;
    }

    private static WavMetadata ReadMetadata(FileStream stream)
    {
        using var reader = new BinaryReader(stream, Encoding.ASCII, leaveOpen: true);
        if (stream.Length < 12)
        {
            throw new InvalidDataException("The WAV file is shorter than a RIFF header.");
        }
        if (ReadFourCc(reader) != "RIFF")
        {
            throw new InvalidDataException("The selected file is not a RIFF WAV file.");
        }

        var riffSize = reader.ReadUInt32();
        if (ReadFourCc(reader) != "WAVE")
        {
            throw new InvalidDataException("The selected file is not a WAVE file.");
        }
        var riffEnd = checked((long)riffSize + 8L);
        if (riffEnd < 12 || riffEnd > stream.Length)
        {
            throw new InvalidDataException("The RIFF container extends past the end of the file.");
        }

        ParsedFormat? format = null;
        long? dataBytes = null;
        while (stream.Position + 8 <= riffEnd)
        {
            var chunkId = ReadFourCc(reader);
            var chunkSize = reader.ReadUInt32();
            var chunkStart = stream.Position;
            var paddedSize = checked((long)chunkSize + (chunkSize & 1U));
            if (chunkStart > riffEnd || paddedSize > riffEnd - chunkStart)
            {
                throw new InvalidDataException($"WAV chunk '{chunkId}' extends past the RIFF container.");
            }

            if (chunkId == "fmt " && format is null)
            {
                var bytesToRead = checked((int)Math.Min(chunkSize, 40U));
                var bytes = reader.ReadBytes(bytesToRead);
                if (bytes.Length != bytesToRead)
                {
                    throw new InvalidDataException("The WAV format chunk is truncated.");
                }
                format = ParseFormat(bytes, chunkSize);
            }
            else if (chunkId == "data" && dataBytes is null)
            {
                dataBytes = chunkSize;
            }

            stream.Position = checked(chunkStart + paddedSize);
        }

        if (format is null || dataBytes is null)
        {
            throw new InvalidDataException("The WAV file is missing required format or audio data.");
        }
        if (dataBytes.Value % format.BlockAlign != 0)
        {
            throw new InvalidDataException("The WAV data chunk contains a partial audio frame.");
        }

        var frames = dataBytes.Value / format.BlockAlign;
        if (frames == 0)
        {
            throw new InvalidDataException("The WAV file contains no audio frames.");
        }
        return new WavMetadata(
            checked((int)format.SampleRate),
            format.Channels,
            format.ContainerBits,
            frames,
            frames / (double)format.SampleRate);
    }

    private static FileStream OpenRead(string path, bool asynchronous = false)
        => new(
            Path.GetFullPath(path),
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            bufferSize: 4096,
            FileOptions.SequentialScan
                | (asynchronous ? FileOptions.Asynchronous : FileOptions.None));

    private static ParsedFormat ParseFormat(byte[] bytes, uint chunkSize)
    {
        if (chunkSize < 16 || bytes.Length < 16)
        {
            throw new InvalidDataException("The WAV format chunk is shorter than 16 bytes.");
        }

        var span = bytes.AsSpan();
        var formatTag = BinaryPrimitives.ReadUInt16LittleEndian(span);
        var channels = BinaryPrimitives.ReadUInt16LittleEndian(span[2..]);
        var sampleRate = BinaryPrimitives.ReadUInt32LittleEndian(span[4..]);
        var blockAlign = BinaryPrimitives.ReadUInt16LittleEndian(span[12..]);
        var containerBits = BinaryPrimitives.ReadUInt16LittleEndian(span[14..]);
        var validBits = containerBits;

        if (formatTag == ExtensibleFormat)
        {
            var extensionSize = bytes.Length >= 18
                ? BinaryPrimitives.ReadUInt16LittleEndian(span[16..])
                : (ushort)0;
            if (chunkSize < 40 || bytes.Length < 40 || extensionSize < 22
                || extensionSize + 18U > chunkSize)
            {
                throw new InvalidDataException("The WAVE_FORMAT_EXTENSIBLE format chunk is incomplete.");
            }

            validBits = BinaryPrimitives.ReadUInt16LittleEndian(span[18..]);
            if (validBits == 0)
            {
                validBits = containerBits;
            }
            if (!span.Slice(28, 12).SequenceEqual(CanonicalSubformatGuidTail))
            {
                throw new InvalidDataException("The extensible WAV uses an unknown subformat GUID.");
            }

            var subformat = BinaryPrimitives.ReadUInt32LittleEndian(span[24..]);
            if (subformat is not (PcmFormat or IeeeFloatFormat))
            {
                throw new InvalidDataException("The extensible WAV subformat is neither PCM nor IEEE float.");
            }
            formatTag = checked((ushort)subformat);
        }

        if (formatTag is not (PcmFormat or IeeeFloatFormat))
        {
            throw new InvalidDataException($"Unsupported WAV format tag: {formatTag}.");
        }
        if (channels == 0 || sampleRate == 0 || blockAlign == 0)
        {
            throw new InvalidDataException("The WAV format contains a zero channel count, sample rate, or block alignment.");
        }
        if (containerBits is not (16 or 24 or 32))
        {
            throw new InvalidDataException($"Unsupported WAV bit depth: {containerBits}.");
        }
        if (formatTag == IeeeFloatFormat && containerBits != 32)
        {
            throw new InvalidDataException("IEEE float WAV files must use 32-bit samples.");
        }
        if (validBits == 0 || validBits > containerBits
            || (formatTag == IeeeFloatFormat && validBits != 32))
        {
            throw new InvalidDataException("The WAV valid-bits-per-sample value is invalid.");
        }

        var expectedBlockAlign = checked((uint)channels * (uint)(containerBits / 8));
        if (expectedBlockAlign > ushort.MaxValue || blockAlign != expectedBlockAlign)
        {
            throw new InvalidDataException("The WAV block alignment does not match its channels and bit depth.");
        }
        return new ParsedFormat(channels, sampleRate, blockAlign, containerBits);
    }

    private static string ReadFourCc(BinaryReader reader)
    {
        var bytes = reader.ReadBytes(4);
        if (bytes.Length != 4)
        {
            throw new InvalidDataException("The WAV file contains a truncated FourCC.");
        }
        return Encoding.ASCII.GetString(bytes);
    }

    private static FileIdentity? TryGetIdentity(SafeFileHandle handle)
    {
        if (!GetFileInformationByHandle(handle, out var information))
        {
            return null;
        }

        return new FileIdentity(
            information.VolumeSerialNumber,
            ((ulong)information.FileIndexHigh << 32) | information.FileIndexLow);
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetFileInformationByHandle(
        SafeFileHandle file,
        out ByHandleFileInformation information);

    [StructLayout(LayoutKind.Sequential)]
    private struct ByHandleFileInformation
    {
        public uint FileAttributes;
        public FileTime CreationTime;
        public FileTime LastAccessTime;
        public FileTime LastWriteTime;
        public uint VolumeSerialNumber;
        public uint FileSizeHigh;
        public uint FileSizeLow;
        public uint NumberOfLinks;
        public uint FileIndexHigh;
        public uint FileIndexLow;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct FileTime
    {
        public uint LowDateTime;
        public uint HighDateTime;
    }

    private sealed record ParsedFormat(
        ushort Channels,
        uint SampleRate,
        ushort BlockAlign,
        ushort ContainerBits);
}
