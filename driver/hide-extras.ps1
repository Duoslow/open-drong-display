# Hides the dongle's USB drive (interface 3, holds the vendor installer) and its audio device (interface 1) on every
# USB port, using Windows' device installation policy "Prevent installation of devices that match these device IDs".
# Only these two interfaces of USB 1CBE:0003 are affected; the display (interface 0) and all other devices are not.
# Called by install.ps1 (and with -Undo by install.ps1 -Uninstall); can also be run on its own, elevated.
#   -Undo removes the policy entries and re-enables the interfaces.
param([switch]$Undo)
$ErrorActionPreference = 'Stop'
$Ids = 'USB\VID_1CBE&PID_0003&MI_01', 'USB\VID_1CBE&PID_0003&MI_03'
$Policy = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\DeviceInstall\Restrictions'
$List = "$Policy\DenyDeviceIDs"

function Get-Interfaces { Get-PnpDevice | Where-Object { $id = $_.InstanceId; $Ids | Where-Object { $id -like "$_\*" } } }

if ($Undo) {
    if (Test-Path $List) {
        $props = (Get-Item $List).Property
        foreach ($p in $props) { if ($Ids -contains (Get-ItemPropertyValue $List $p)) { Remove-ItemProperty $List $p } }
        if (-not (Get-Item $List).Property) {
            Remove-Item $List
            Remove-ItemProperty $Policy -Name DenyDeviceIDs, DenyDeviceIDsRetroactive -ErrorAction SilentlyContinue
        }
    }
    Get-Interfaces | Where-Object Present | ForEach-Object { Enable-PnpDevice -InstanceId $_.InstanceId -Confirm:$false }
    Write-Output 'policy removed; replug the dongle to get its USB drive and audio device back'
    return
}

New-Item $List -Force | Out-Null
Set-ItemProperty $Policy -Name DenyDeviceIDs -Value 1 -Type DWord
# keep other entries someone may already have in the list; append ours under free numbers
$existing = @((Get-Item $List).Property | ForEach-Object { Get-ItemPropertyValue $List $_ })
$n = ((Get-Item $List).Property | ForEach-Object { [int]$_ } | Measure-Object -Maximum).Maximum
foreach ($id in $Ids) {
    if ($existing -notcontains $id) { $n++; Set-ItemProperty $List -Name "$n" -Value $id -Type String }
}
# interfaces that are already installed on this port: switch them off now
Get-Interfaces | Where-Object { $_.Present -and $_.Status -eq 'OK' } | ForEach-Object { Disable-PnpDevice -InstanceId $_.InstanceId -Confirm:$false }
Write-Output 'the dongle''s USB drive and audio device are hidden on every port'
