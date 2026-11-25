# Simplewall Fork

Control which Windows apps can connect to the network, then inspect what they tried to do. This fork of [Henry++’s simplewall](https://github.com/henrypp/simplewall) focuses on keeping large app lists responsive and making packet history easier to use.

[Download the Windows x64 build](https://github.com/Mainkill1/simplewall/releases/latest) · [Release notes](https://github.com/Mainkill1/simplewall/releases) · [Report an issue](https://github.com/Mainkill1/simplewall/issues)

## What this fork adds

- **More responsive app lists.** Caches rule lookups, prepares sort keys once per row, and moves executable availability checks off the UI thread. Refresh rechecks app availability without rebuilding firewall filters.
- **Packet history by app.** A split view shows apps on the left and traffic for the selected app on the right. History survives restarts. View individual events or group repeats, filter by action or direction, sort columns, and right-click to copy a cell.
- **More control from the app list.** Monitor allowed traffic for an app, inspect its rules and executable properties, open its file location, or suspend, resume, or terminate its process.
- **Timed discovery.** The tray’s **Allow new apps** menu offers 1-, 5-, and 15-minute discovery windows, with options to allow newly discovered apps temporarily or permanently.
- **Better connection attribution.** A **ServiceID** column identifies the Windows service behind an `svchost.exe` connection when it can be determined. Executables on network shares are supported.
- **UI fixes.** Corrects notification and logging toggle states, fixes blank rules tabs introduced in the fork, and improves packet-log redraw and live refresh. The separate issue where a new-app alert can stall during traffic bursts is [still under investigation](docs/upstream-candidates/New-app-notification-stalls.md).

Example of the overhauled Packet Log
<img width="1679" height="945" alt="image" src="https://github.com/user-attachments/assets/3ebbc01c-88aa-4d8f-b310-ca0661f003fa" />


The existing simplewall features are still here: custom app and global rules, IPv4/IPv6 filtering, Windows service and WSL support, blocklists, and packet notifications.

## Install and use

1. Download the x64 ZIP from [this fork’s releases](https://github.com/Mainkill1/simplewall/releases) and check it against the included `SHA256SUMS.txt`.
2. If upgrading, back up your profile and packet history. Close simplewall before copying the history database or replacing the executable.
3. Run `simplewall.exe` as administrator. For portable mode, create `simplewall.ini` next to the executable or move your existing copy from `%APPDATA%\Henry++\simplewall`.
4. When enabling filters, choose **Permanent** to keep them until manually disabled or **Temporary** to clear them on reboot.

This fork uses the [Windows Filtering Platform](https://learn.microsoft.com/en-us/windows/win32/fwp/windows-filtering-platform-start-page). It is not a front end for Windows Defender Firewall and does not require turning it off. Installed filters can remain active after the app closes. **Before uninstalling or removing the portable files, open simplewall and click Disable filters.**

## Documentation

- [Upstream simplewall guide](https://github.com/henrypp/simplewall#rules-editor): rule syntax, command-line options, and troubleshooting.
- [Fork improvements and upstream candidates](docs/upstream-candidates/Home.md): implementation notes, source comparisons, and changes that could be submitted separately.

## Support development

[Support this fork on Ko-fi](https://ko-fi.com/mainkill1)

## Credits and license

Based on [simplewall by Henry++](https://github.com/henrypp/simplewall). Licensed under the [GNU General Public License v3.0](LICENSE).
