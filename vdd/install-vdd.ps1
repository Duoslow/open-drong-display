# Installs the open-source Virtual Display Driver (Root\MttVDD) as one virtual 1920x1080@60 monitor.
# Equivalent of "devcon install MttVDD.inf Root\MttVDD". Run elevated.
param([string]$Log = "$PSScriptRoot\install-vdd.log", [switch]$Apply)
$ErrorActionPreference = 'Stop'
Start-Transcript -Path $Log -Force | Out-Null
try {
    # The driver reads its settings from this fixed path
    New-Item -ItemType Directory -Force C:\VirtualDisplayDriver | Out-Null
    Copy-Item "$PSScriptRoot\vdd_settings.xml", "$PSScriptRoot\user_edid.bin" C:\VirtualDisplayDriver\ -Force
    if ($Apply) {
        # settings/EDID changed on an installed driver: restart the device so they take effect
        Get-PnpDevice | Where-Object { $_.HardwareID -contains 'Root\MttVDD' } | ForEach-Object { pnputil /restart-device $_.InstanceId }
        return
    }

    if (Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains 'Root\MttVDD' }) {
        Write-Output 'Root\MttVDD device already exists; updating driver only'
        pnputil /add-driver "$PSScriptRoot\MttVDD.inf" /install
        return
    }

    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class RootDev {
    [StructLayout(LayoutKind.Sequential)]
    public struct SP_DEVINFO_DATA { public int cbSize; public Guid ClassGuid; public int DevInst; public IntPtr Reserved; }
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool SetupDiGetINFClass(string inf, out Guid guid, System.Text.StringBuilder cls, int size, out int req);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid guid, IntPtr hwnd);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool SetupDiCreateDeviceInfo(IntPtr set, string name, ref Guid guid, string desc, IntPtr hwnd, int flags, ref SP_DEVINFO_DATA data);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool SetupDiSetDeviceRegistryProperty(IntPtr set, ref SP_DEVINFO_DATA data, int prop, byte[] buf, int size);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiCallClassInstaller(int fn, IntPtr set, ref SP_DEVINFO_DATA data);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
    [DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool UpdateDriverForPlugAndPlayDevices(IntPtr hwnd, string hwid, string inf, int flags, out bool reboot);

    const int DICD_GENERATE_ID = 1, SPDRP_HARDWAREID = 1, DIF_REGISTERDEVICE = 0x19, INSTALLFLAG_FORCE = 1;

    public static string Install(string inf, string hwid) {
        Guid guid; int req; var cls = new System.Text.StringBuilder(256);
        if (!SetupDiGetINFClass(inf, out guid, cls, 256, out req)) throw new Exception("SetupDiGetINFClass " + Marshal.GetLastWin32Error());
        IntPtr set = SetupDiCreateDeviceInfoList(ref guid, IntPtr.Zero);
        try {
            var d = new SP_DEVINFO_DATA(); d.cbSize = Marshal.SizeOf(d);
            if (!SetupDiCreateDeviceInfo(set, cls.ToString(), ref guid, null, IntPtr.Zero, DICD_GENERATE_ID, ref d)) throw new Exception("SetupDiCreateDeviceInfo " + Marshal.GetLastWin32Error());
            byte[] ids = System.Text.Encoding.Unicode.GetBytes(hwid + "\0\0");
            if (!SetupDiSetDeviceRegistryProperty(set, ref d, SPDRP_HARDWAREID, ids, ids.Length)) throw new Exception("SetHardwareID " + Marshal.GetLastWin32Error());
            if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, ref d)) throw new Exception("DIF_REGISTERDEVICE " + Marshal.GetLastWin32Error());
        } finally { SetupDiDestroyDeviceInfoList(set); }
        bool reboot;
        if (!UpdateDriverForPlugAndPlayDevices(IntPtr.Zero, hwid, inf, INSTALLFLAG_FORCE, out reboot)) throw new Exception("UpdateDriverForPlugAndPlayDevices " + Marshal.GetLastWin32Error());
        return reboot ? "installed (reboot required)" : "installed";
    }
}
'@
    Write-Output ([RootDev]::Install("$PSScriptRoot\MttVDD.inf", 'Root\MttVDD'))
}
catch { Write-Output "ERROR: $_" }
finally { Stop-Transcript | Out-Null }
