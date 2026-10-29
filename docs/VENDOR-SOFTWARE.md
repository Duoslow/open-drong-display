# The vendor software

The dongle carries a small read-only USB drive with `Drong Display.exe` and asks you to install it. This document
records what that installer contains, found by unpacking it and statically analysing the files (nothing was
executed). It explains why this project exists; it is not a malware verdict.

## Installer

| | |
|---|---|
| File | `Drong Display.exe`, 8,641,957 bytes, version 1.0.8, "Copyright (C) 2024 Drong" |
| SHA-256 | `35C9BA5218BDA38D743AB9DA18B850344C21B2643EC8B8115917FA34A3F77296` ([VirusTotal: 9/71 engines flag it](https://www.virustotal.com/gui/file/35c9ba5218bda38d743ab9da18b850344c21b2643ec8b8115917fa34a3f77296/detection)) |
| Signature | none |
| Type | Advanced Installer bootstrapper with two embedded 7z archives (first 0x200 bytes XOR 0xFF): `FILES.7z` and `Drong Display.7z` (an MSI) |

### Installed files

| File | Signature | What it is |
|---|---|---|
| `IddSampleDriver.dll` / `.inf` / `.cat` | DLL: Microsoft *WDK test* certificate; catalog: self-signed `CN=DrongCert` | The display driver: Microsoft's IndirectDisplay sample with FFmpeg 6.1 and x264 linked in, binding to `USB\VID_1CBE&PID_0003&MI_00` |
| `Drong.cer` (in the MSI) | — | `CN=DrongCert` (thumbprint `FBC274FB2333CFD7FE33443709E27A2E32668E80`, code signing only), added to the trusted certificate stores so the driver loads |
| `DrongUsbService.exe` | `DrongCert` | Windows service, named pipe `\\.\pipe\Drongpipe`; runs `a.bat` |
| `a.bat` | — | starts `CheckPackageRun.exe` elevated |
| `CheckPackageRun.exe` | `DrongCert` | downloads an advert video (`C:\Program Files\Drong\Drong Display\Advert.h264`) and firmware |

## Network use

The driver itself (not only the helper programs) contains these endpoints on `ds.dronghootin.com.cn`:

```
/api/drong/authProduct                                   "authorized" check, result is sent to the dongle (0x55/0x54)
/api/drong/isCompatibility
/admin-api/system/product-online/saveOrRefreshProductOnline
/admin-api/system/product-online/uploadErrorLog
/admin-api/system/productVersion/getAdvertising
/admin-api/system/productVersion/upgradeFirmware
/admin-api/system/productPreview/upload
```

Fields it assembles for these requests include the dongle's serial/uuid, model, firmware version, usage counter,
advert count, and the PC's manufacturer, model and GPU (queried through WMI `Win32_ComputerSystem`). What
`productPreview/upload` uploads was not determined.

The dongle firmware itself also asks the PC to fetch `http://8.134.59.18/drongscreen/abs/abs9/product_sabbat_dual.abs`
(event `0x71`).

## Summary

- Unsigned installer; driver signed with a test certificate plus a self-signed root that must be trusted
  machine-wide.
- A background service that launches an elevated helper.
- Telemetry and an online "authorization" in the display driver; adverts and firmware downloaded from the vendor's
  server.
- FFmpeg/x264 (GPL) are linked into the driver; no source code is offered.

This project needs none of it: the protocol is documented in [PROTOCOL.md](PROTOCOL.md) and the replacement driver
talks to nothing but the dongle.
