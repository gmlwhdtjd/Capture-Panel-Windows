using System.IO;
using System.Text;
using CapturePanel.App.Models;

namespace CapturePanel.App.Services;

public static class WavMetadataReader
{
    public static WavMetadata Read(string path)
    {
        using var stream = File.Open(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        using var reader = new BinaryReader(stream, Encoding.ASCII, leaveOpen: true);

        if (ReadFourCc(reader) != "RIFF")
        {
            throw new InvalidDataException("The selected file is not a RIFF WAV file.");
        }

        _ = reader.ReadUInt32();
        if (ReadFourCc(reader) != "WAVE")
        {
            throw new InvalidDataException("The selected file is not a WAVE file.");
        }

        ushort channels = 0;
        uint sampleRate = 0;
        ushort blockAlign = 0;
        ushort bitsPerSample = 0;
        long dataBytes = -1;

        while (stream.Position + 8 <= stream.Length)
        {
            var chunkId = ReadFourCc(reader);
            var chunkSize = reader.ReadUInt32();
            var chunkStart = stream.Position;
            var chunkEnd = checked(chunkStart + chunkSize);
            if (chunkEnd > stream.Length)
            {
                throw new InvalidDataException($"WAV chunk '{chunkId}' extends past the end of the file.");
            }

            if (chunkId == "fmt ")
            {
                if (chunkSize < 16)
                {
                    throw new InvalidDataException("The WAV format chunk is incomplete.");
                }

                var formatTag = reader.ReadUInt16();
                channels = reader.ReadUInt16();
                sampleRate = reader.ReadUInt32();
                _ = reader.ReadUInt32();
                blockAlign = reader.ReadUInt16();
                bitsPerSample = reader.ReadUInt16();
                if (formatTag is not (1 or 3 or 0xFFFE))
                {
                    throw new InvalidDataException($"Unsupported WAV format tag: {formatTag}.");
                }
            }
            else if (chunkId == "data")
            {
                dataBytes = chunkSize;
            }

            stream.Position = chunkEnd + (chunkSize & 1U);
        }

        if (channels == 0 || sampleRate == 0 || blockAlign == 0 || bitsPerSample == 0 || dataBytes < 0)
        {
            throw new InvalidDataException("The WAV file is missing required format or audio data.");
        }

        var frames = dataBytes / blockAlign;
        return new WavMetadata(
            checked((int)sampleRate),
            channels,
            bitsPerSample,
            frames,
            frames / (double)sampleRate);
    }

    private static string ReadFourCc(BinaryReader reader)
        => Encoding.ASCII.GetString(reader.ReadBytes(4));
}
