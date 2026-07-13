using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace CapturePanel.App.Services;

internal sealed class WorkerProcessJob : IDisposable
{
    private const uint KillOnJobClose = 0x00002000;
    private readonly SafeFileHandle _job;

    private WorkerProcessJob(SafeFileHandle job)
    {
        _job = job;
    }

    public static WorkerProcessJob Attach(Process process)
    {
        var nativeHandle = CreateJobObject(IntPtr.Zero, null);
        if (nativeHandle == IntPtr.Zero || nativeHandle == new IntPtr(-1))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not create the worker process job.");
        }

        var handle = new SafeFileHandle(nativeHandle, ownsHandle: true);
        try
        {
            var information = new JobObjectExtendedLimitInformation
            {
                BasicLimitInformation = new JobObjectBasicLimitInformation
                {
                    LimitFlags = KillOnJobClose,
                },
            };
            if (!SetInformationJobObject(
                    handle,
                    JobObjectInfoType.ExtendedLimitInformation,
                    ref information,
                    (uint)Marshal.SizeOf<JobObjectExtendedLimitInformation>()))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not configure the worker process job.");
            }
            if (!AssignProcessToJobObject(handle, process.Handle))
            {
                if (process.HasExited)
                {
                    return new WorkerProcessJob(handle);
                }
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not isolate the native audio worker.");
            }
            return new WorkerProcessJob(handle);
        }
        catch
        {
            handle.Dispose();
            throw;
        }
    }

    public void Terminate()
    {
        if (_job.IsInvalid || _job.IsClosed)
        {
            return;
        }

        _ = TerminateJobObject(_job, 1);
    }

    public void Dispose() => _job.Dispose();

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateJobObject(IntPtr securityAttributes, string? name);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetInformationJobObject(
        SafeFileHandle job,
        JobObjectInfoType informationClass,
        ref JobObjectExtendedLimitInformation information,
        uint informationLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool AssignProcessToJobObject(SafeFileHandle job, IntPtr process);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool TerminateJobObject(SafeFileHandle job, uint exitCode);

    private enum JobObjectInfoType
    {
        ExtendedLimitInformation = 9,
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct JobObjectBasicLimitInformation
    {
        public long PerProcessUserTimeLimit;
        public long PerJobUserTimeLimit;
        public uint LimitFlags;
        public nuint MinimumWorkingSetSize;
        public nuint MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public nuint Affinity;
        public uint PriorityClass;
        public uint SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct IoCounters
    {
        public ulong ReadOperationCount;
        public ulong WriteOperationCount;
        public ulong OtherOperationCount;
        public ulong ReadTransferCount;
        public ulong WriteTransferCount;
        public ulong OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct JobObjectExtendedLimitInformation
    {
        public JobObjectBasicLimitInformation BasicLimitInformation;
        public IoCounters IoInfo;
        public nuint ProcessMemoryLimit;
        public nuint JobMemoryLimit;
        public nuint PeakProcessMemoryUsed;
        public nuint PeakJobMemoryUsed;
    }
}
