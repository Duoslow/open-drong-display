# Signs and installs DrongIdd (run elevated after build.bat).
#
#   1. creates a one-off code-signing certificate, signs DrongIdd.dll and the catalog with it
#   2. trusts that certificate (LocalMachine Root + TrustedPublisher) and then DELETES its private key,
#      so nothing else can ever be signed with it
#   3. installs the driver package and forces it onto the dongle's display interface (USB\VID_1CBE&PID_0003&MI_00)
#   4. disables the Virtual Display Driver (tools\virtual-display) if present, which this driver replaces
#
#   -Uninstall removes the driver package and the certificates again and re-enables the Virtual Display Driver.
#
#   Needs the Windows Driver Kit 10.0.26100 (signtool, Inf2Cat) and the dongle plugged in.
param([switch]$Uninstall, [string]$Log = "$PSScriptRoot\out\install.log")
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force "$PSScriptRoot\out" | Out-Null
Start-Transcript -Path $Log -Force | Out-Null
$Subject = 'CN=drong-display local driver signing'
$HwId = 'USB\VID_1CBE&PID_0003&MI_00'
$Kit = "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0"
try {
    function Remove-OurCerts([string]$Keep) {
        foreach ($store in 'Root', 'TrustedPublisher', 'My') {
            Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -eq $Subject -and $_.Thumbprint -ne $Keep } |
                ForEach-Object { Write-Output "removing old certificate $($_.Thumbprint) from $store"; Remove-Item $_.PSPath -DeleteKey -ErrorAction SilentlyContinue; Remove-Item $_.PSPath -ErrorAction SilentlyContinue }
        }
    }
    function Get-OurPublishedInfs {
        # oemNN.inf names of our packages in the driver store (DISM objects: independent of the UI language)
        Get-WindowsDriver -Online | Where-Object { (Split-Path $_.OriginalFileName -Leaf) -eq 'drongidd.inf' } |
            ForEach-Object { $_.Driver }
    }
    $vdd = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains 'Root\MttVDD' }

    if ($Uninstall) {
        foreach ($inf in Get-OurPublishedInfs) { Write-Output "removing driver package $inf"; pnputil /delete-driver $inf /uninstall /force }
        Remove-OurCerts -Keep ''
        if ($vdd) { Enable-PnpDevice -InstanceId $vdd.InstanceId -Confirm:$false; Write-Output 'Virtual Display Driver re-enabled' }
        Write-Output 'uninstalled. To use drongcast.exe again, re-bind WinUSB to "windows mirroring interface" in Device Manager.'
        return
    }

    $pkg = "$PSScriptRoot\out\package"
    if (-not (Test-Path "$PSScriptRoot\out\DrongIdd.dll")) { throw 'out\DrongIdd.dll missing - run build.bat first' }
    Remove-Item $pkg -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory $pkg | Out-Null
    Copy-Item "$PSScriptRoot\out\DrongIdd.dll" $pkg
    # every install gets a newer DriverVer, otherwise Windows keeps using the package already in the driver store
    $now = Get-Date
    $ver = '1.{0}.{1}.{2}' -f $now.ToString('yyMM'), $now.Day, $now.ToString('HHmm')
    $date = $now.ToString('MM/dd/yyyy', [Globalization.CultureInfo]::InvariantCulture) # INF needs '/' whatever the locale
    (Get-Content "$PSScriptRoot\DrongIdd.inf") -replace '^DriverVer\s*=.*$', ('DriverVer = {0},{1}' -f $date, $ver) |
        Set-Content "$pkg\DrongIdd.inf" -Encoding ASCII
    Write-Output "driver version $ver"

    # the user-mode sender (tools\sender) would hold the USB interface: stop it cleanly
    if (Get-Process drongcast -ErrorAction SilentlyContinue) {
        New-Item -ItemType File -Force "$PSScriptRoot\..\tools\sender\out\stop" | Out-Null
        Start-Sleep 3
        Get-Process drongcast -ErrorAction SilentlyContinue | Stop-Process -Force
    }

    # 1. one-off signing certificate
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject -CertStoreLocation Cert:\LocalMachine\My `
        -KeyExportPolicy NonExportable -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(10)
    Write-Output "signing certificate $($cert.Thumbprint)"
    & "$Kit\x64\signtool.exe" sign /fd SHA256 /sm /s My /sha1 $cert.Thumbprint "$pkg\DrongIdd.dll"
    if ($LASTEXITCODE) { throw 'signing DrongIdd.dll failed' }
    & "$Kit\x86\Inf2Cat.exe" /driver:$pkg /os:10_VB_X64,10_X64 /uselocaltime
    if ($LASTEXITCODE) { throw 'Inf2Cat failed' }
    & "$Kit\x64\signtool.exe" sign /fd SHA256 /sm /s My /sha1 $cert.Thumbprint "$pkg\DrongIdd.cat"
    if ($LASTEXITCODE) { throw 'signing DrongIdd.cat failed' }

    # 2. trust the public part, then destroy the private key
    $cer = "$pkg\drong-display-driver.cer"
    Export-Certificate -Cert $cert -FilePath $cer | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
    Remove-Item "Cert:\LocalMachine\My\$($cert.Thumbprint)" -DeleteKey
    Remove-OurCerts -Keep $cert.Thumbprint
    Write-Output 'certificate trusted, private key deleted'

    # log folder the driver (running as LOCAL SERVICE inside WUDFHost) can write to
    New-Item -ItemType Directory -Force C:\ProgramData\drong-display | Out-Null
    icacls C:\ProgramData\drong-display /grant '*S-1-5-19:(OI)(CI)M' '*S-1-5-84-0-0-0-0-0:(OI)(CI)M' | Out-Null

    # 3. install (older packages are removed once the new one is on the device)
    $old = @(Get-OurPublishedInfs)
    pnputil /add-driver "$pkg\DrongIdd.inf"
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class NewDev {
    [DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool UpdateDriverForPlugAndPlayDevices(IntPtr hwnd, string hwid, string inf, int flags, out bool reboot);
}
'@
    $reboot = $false
    if (-not [NewDev]::UpdateDriverForPlugAndPlayDevices([IntPtr]::Zero, $HwId, "$pkg\DrongIdd.inf", 1, [ref]$reboot)) {
        $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "UpdateDriverForPlugAndPlayDevices failed ($err) - is the dongle plugged in?"
    }
    Write-Output ("driver installed on the dongle" + ($(if ($reboot) { ' (reboot required)' } else { '' })))
    $inUse = (Get-PnpDevice -PresentOnly | Where-Object { $_.HardwareID -contains $HwId } | Get-PnpDeviceProperty -KeyName DEVPKEY_Device_DriverInfPath).Data
    Write-Output "dongle now uses $inUse"
    foreach ($inf in $old) { if ($inf -ne $inUse) { Write-Output "removing superseded package $inf"; pnputil /delete-driver $inf | Out-Null } }

    # 4. one USB-Monitor is enough
    if ($vdd -and $vdd.Status -eq 'OK') { Disable-PnpDevice -InstanceId $vdd.InstanceId -Confirm:$false; Write-Output 'Virtual Display Driver disabled' }
}
catch { Write-Output "ERROR: $_" }
finally { Stop-Transcript | Out-Null }
