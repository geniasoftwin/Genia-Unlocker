#requires -Version 5.1
<#
Genia Unlocker Test Lab
Safe lock-scenario emulator for Genia Unlocker.

Creates test objects only under %TEMP%\GeniaUnlocker-TestLab\<session>.
Normal worker processes are stopped automatically on clean exit.
The elevated worker may require Genia Unlocker / Task Manager (Admin) to stop.
#>

[CmdletBinding()]
param(
    [ValidateSet('', 'FileExclusive', 'FileNoDelete', 'DirectoryNoDelete', 'MappedFile', 'ReopenFile')]
    [string]$Worker = '',
    [string]$Target = '',
    [string]$Marker = ''
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# ---------------- Native helpers ----------------
$nativeCode = @"
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class GeniaTestNative {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern SafeFileHandle CreateFile(
        string lpFileName,
        uint dwDesiredAccess,
        uint dwShareMode,
        IntPtr lpSecurityAttributes,
        uint dwCreationDisposition,
        uint dwFlagsAndAttributes,
        IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool GetFileInformationByHandle(
        SafeFileHandle hFile,
        out BY_HANDLE_FILE_INFORMATION info);

    [StructLayout(LayoutKind.Sequential)]
    public struct FILETIME {
        public uint dwLowDateTime;
        public uint dwHighDateTime;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct BY_HANDLE_FILE_INFORMATION {
        public uint dwFileAttributes;
        public FILETIME ftCreationTime;
        public FILETIME ftLastAccessTime;
        public FILETIME ftLastWriteTime;
        public uint dwVolumeSerialNumber;
        public uint nFileSizeHigh;
        public uint nFileSizeLow;
        public uint nNumberOfLinks;
        public uint nFileIndexHigh;
        public uint nFileIndexLow;
    }
}
"@

if (-not ("GeniaTestNative" -as [type])) {
    Add-Type -TypeDefinition $nativeCode -Language CSharp
}

function Write-Marker {
    param([string]$Mode, [string]$Path)
    if ([string]::IsNullOrWhiteSpace($Marker)) { return }

    $obj = [ordered]@{
        PID       = $PID
        Mode      = $Mode
        Target    = $Path
        Elevated  = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).
                    IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
        StartedAt = (Get-Date).ToString("s")
    }
    $obj | ConvertTo-Json | Set-Content -LiteralPath $Marker -Encoding UTF8
}

function Invoke-Worker {
    param([string]$Mode, [string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "Worker target is empty."
    }

    Write-Marker -Mode $Mode -Path $Path

    switch ($Mode) {
        'FileExclusive' {
            $fs = [System.IO.File]::Open(
                $Path,
                [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::ReadWrite,
                [System.IO.FileShare]::None
            )
            try {
                while ($true) { Start-Sleep -Seconds 1 }
            }
            finally { $fs.Dispose() }
        }

        'FileNoDelete' {
            # Other processes may read/write, but delete/rename sharing is denied.
            $fs = New-Object System.IO.FileStream(
                $Path,
                [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::ReadWrite,
                ([System.IO.FileShare]::ReadWrite)
            )
            try {
                while ($true) { Start-Sleep -Seconds 1 }
            }
            finally { $fs.Dispose() }
        }

        'DirectoryNoDelete' {
            $GENERIC_READ = [uint32]0x80000000
            $FILE_SHARE_READ  = [uint32]0x00000001
            $FILE_SHARE_WRITE = [uint32]0x00000002
            $OPEN_EXISTING = [uint32]3
            $FILE_FLAG_BACKUP_SEMANTICS = [uint32]0x02000000

            $h = [GeniaTestNative]::CreateFile(
                $Path,
                $GENERIC_READ,
                ($FILE_SHARE_READ -bor $FILE_SHARE_WRITE), # deliberately NO FILE_SHARE_DELETE
                [IntPtr]::Zero,
                $OPEN_EXISTING,
                $FILE_FLAG_BACKUP_SEMANTICS,
                [IntPtr]::Zero
            )

            if ($h.IsInvalid) {
                $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
                throw "CreateFile(directory) failed. Win32 error: $err"
            }

            try {
                while ($true) { Start-Sleep -Seconds 1 }
            }
            finally { $h.Dispose() }
        }

        'MappedFile' {
            $fs = New-Object System.IO.FileStream(
                $Path,
                [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::ReadWrite,
                ([System.IO.FileShare]::ReadWrite)
            )
            $mmf = [System.IO.MemoryMappedFiles.MemoryMappedFile]::CreateFromFile(
                $fs,
                $null,
                0,
                [System.IO.MemoryMappedFiles.MemoryMappedFileAccess]::ReadWrite,
                [System.IO.HandleInheritability]::None,
                $false
            )
            $view = $mmf.CreateViewAccessor()
            try {
                while ($true) { Start-Sleep -Seconds 1 }
            }
            finally {
                $view.Dispose()
                $mmf.Dispose()
                $fs.Dispose()
            }
        }

        'ReopenFile' {
            # Simulates a stubborn service/app that reacquires a handle after it is closed externally.
            $fs = $null
            try {
                while ($true) {
                    if ($null -eq $fs) {
                        try {
                            $fs = New-Object System.IO.FileStream(
                                $Path,
                                [System.IO.FileMode]::Open,
                                [System.IO.FileAccess]::ReadWrite,
                                ([System.IO.FileShare]::ReadWrite)
                            )
                        }
                        catch {
                            Start-Sleep -Milliseconds 150
                            continue
                        }
                    }

                    try {
                        $fs.Flush()
                        Start-Sleep -Milliseconds 250
                    }
                    catch {
                        try { $fs.Dispose() } catch {}
                        $fs = $null
                        Start-Sleep -Milliseconds 100
                    }
                }
            }
            finally {
                if ($null -ne $fs) {
                    try { $fs.Dispose() } catch {}
                }
            }
        }
    }
}

# Worker mode: no menu.
if ($Worker) {
    Invoke-Worker -Mode $Worker -Path $Target
    exit
}

# ---------------- Controller ----------------

$sessionId = (Get-Date -Format 'yyyyMMdd-HHmmss')
$Root = Join-Path $env:TEMP "GeniaUnlocker-TestLab\$sessionId"
$Markers = Join-Path $Root "_markers"
New-Item -ItemType Directory -Path $Root -Force | Out-Null
New-Item -ItemType Directory -Path $Markers -Force | Out-Null

$script:Workers = New-Object System.Collections.ArrayList

function New-TestFile {
    param(
        [string]$Name,
        [int]$SizeKB = 16
    )

    $path = Join-Path $Root $Name
    $dir = Split-Path -Parent $path
    New-Item -ItemType Directory -Path $dir -Force | Out-Null

    $bytes = New-Object byte[] ($SizeKB * 1024)
    (New-Object System.Random).NextBytes($bytes)
    [System.IO.File]::WriteAllBytes($path, $bytes)
    return $path
}

function Start-LockWorker {
    param(
        [string]$Mode,
        [string]$Path,
        [switch]$Elevated
    )

    $marker = Join-Path $Markers ("{0}-{1}.json" -f $Mode, ([guid]::NewGuid().ToString('N')))
    $exe = (Get-Command powershell.exe).Source

    $args = @(
        '-NoLogo'
        '-NoProfile'
        '-ExecutionPolicy', 'Bypass'
        '-File', ('"{0}"' -f $PSCommandPath)
        '-Worker', $Mode
        '-Target', ('"{0}"' -f $Path)
        '-Marker', ('"{0}"' -f $marker)
    ) -join ' '

    $sp = @{
        FilePath     = $exe
        ArgumentList = $args
        PassThru     = $true
        WindowStyle  = 'Hidden'
    }

    if ($Elevated) {
        $sp.Verb = 'RunAs'
    }

    $p = Start-Process @sp

    $null = $script:Workers.Add([pscustomobject]@{
        PID      = $p.Id
        Mode     = $Mode
        Target   = $Path
        Elevated = [bool]$Elevated
        Marker   = $marker
    })

    Start-Sleep -Milliseconds 500
    return $p
}

function Show-Scenario {
    param(
        [string]$Title,
        [string]$Path,
        [int[]]$Pids
    )

    Write-Host ""
    Write-Host "=== $Title ===" -ForegroundColor Cyan
    Write-Host "Target: $Path"
    Write-Host ("PID(s): " + ($Pids -join ', '))
    Write-Host "Use this Target in Genia Unlocker." -ForegroundColor Yellow
}

function Start-Scenario1 {
    $f = New-TestFile '01-Exclusive\exclusive-lock.txt'
    $p = Start-LockWorker -Mode FileExclusive -Path $f
    Show-Scenario '1. Exclusive file handle' $f @($p.Id)
}

function Start-Scenario2 {
    $f = New-TestFile '02-NoDelete\delete-blocked.txt'
    $p = Start-LockWorker -Mode FileNoDelete -Path $f
    Show-Scenario '2. File allows I/O but blocks delete/rename' $f @($p.Id)
}

function Start-Scenario3 {
    $f = New-TestFile '03-MultiPID\shared-target.txt'
    $p1 = Start-LockWorker -Mode FileNoDelete -Path $f
    $p2 = Start-LockWorker -Mode FileNoDelete -Path $f
    $p3 = Start-LockWorker -Mode FileNoDelete -Path $f
    Show-Scenario '3. Same target held by multiple processes' $f @($p1.Id, $p2.Id, $p3.Id)
}

function Start-Scenario4 {
    $d = Join-Path $Root '04-DirectoryHandle\LockedFolder'
    New-Item -ItemType Directory -Path $d -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $d 'inside.txt') -Value 'Genia Unlocker directory lock test'
    $p = Start-LockWorker -Mode DirectoryNoDelete -Path $d
    Show-Scenario '4. Directory handle blocks folder deletion/rename' $d @($p.Id)
}

function Start-Scenario5 {
    $f = New-TestFile '05-Mapped\mapped-file.bin' 256
    $p = Start-LockWorker -Mode MappedFile -Path $f
    Show-Scenario '5. Memory-mapped file' $f @($p.Id)
}

function Start-Scenario6 {
    $f = New-TestFile '06-Reopen\reopen-after-unlock.txt'
    $p = Start-LockWorker -Mode ReopenFile -Path $f
    Show-Scenario '6. Stubborn process reopens handle after external close' $f @($p.Id)
}

function Start-Scenario7 {
    $f = New-TestFile '07-Elevated\elevated-lock.txt'
    Write-Host ""
    Write-Host "Windows will show a UAC prompt for the lock-holder process." -ForegroundColor Yellow
    $p = Start-LockWorker -Mode FileExclusive -Path $f -Elevated
    Show-Scenario '7. Elevated process / Retry Admin test' $f @($p.Id)
}

function Start-StressPack {
    Write-Host ""
    Write-Host "Creating combined stress pack..." -ForegroundColor Cyan

    Start-Scenario1
    Start-Scenario3
    Start-Scenario4
    Start-Scenario5
    Start-Scenario6

    $folder = Join-Path $Root '08-StressFolder'
    New-Item -ItemType Directory -Path $folder -Force | Out-Null

    $a = New-TestFile '08-StressFolder\a.txt'
    $b = New-TestFile '08-StressFolder\b.bin' 128
    $c = New-TestFile '08-StressFolder\sub\c.txt'

    $p1 = Start-LockWorker -Mode FileExclusive -Path $a
    $p2 = Start-LockWorker -Mode FileNoDelete -Path $b
    $p3 = Start-LockWorker -Mode ReopenFile -Path $c

    Show-Scenario '8. Mixed locked folder tree' $folder @($p1.Id, $p2.Id, $p3.Id)
}

function Show-Status {
    Write-Host ""
    Write-Host "Current Test Lab: $Root" -ForegroundColor Cyan
    Write-Host ""
    if ($script:Workers.Count -eq 0) {
        Write-Host "No workers started."
        return
    }

    $rows = foreach ($w in $script:Workers) {
        $alive = $false
        try {
            $null = Get-Process -Id $w.PID -ErrorAction Stop
            $alive = $true
        } catch {}

        [pscustomobject]@{
            PID      = $w.PID
            Alive    = $alive
            Elevated = $w.Elevated
            Mode     = $w.Mode
            Target   = $w.Target
        }
    }

    $rows | Format-Table -AutoSize
}

function Stop-TestWorkers {
    param([switch]$Quiet)

    foreach ($w in @($script:Workers)) {
        try {
            $p = Get-Process -Id $w.PID -ErrorAction Stop
            Stop-Process -Id $p.Id -Force -ErrorAction Stop
        }
        catch {
            if (-not $Quiet) {
                # Expected for elevated processes when controller is non-admin,
                # or for processes already terminated by Genia Unlocker.
                Write-Host "Could not stop PID $($w.PID) (already gone or elevated)." -ForegroundColor DarkYellow
            }
        }
    }
}

function Clear-TestLab {
    Stop-TestWorkers
    Start-Sleep -Milliseconds 300

    try {
        if (Test-Path -LiteralPath $Root) {
            Remove-Item -LiteralPath $Root -Recurse -Force -ErrorAction Stop
            Write-Host "Test Lab cleaned: $Root" -ForegroundColor Green
        }
    }
    catch {
        Write-Host "Some test objects are still locked. Use Genia Unlocker or stop the elevated worker, then retry Cleanup." -ForegroundColor Yellow
        Write-Host $_.Exception.Message -ForegroundColor DarkYellow
    }
}

function Show-Menu {
    Clear-Host
    Write-Host "Genia Unlocker - PowerShell Test Lab" -ForegroundColor Cyan
    Write-Host "Session: $sessionId"
    Write-Host "Root:    $Root"
    Write-Host ""
    Write-Host "[1] Exclusive file handle"
    Write-Host "[2] Delete/rename blocked, read/write allowed"
    Write-Host "[3] Multiple PIDs locking one file"
    Write-Host "[4] Directory handle / locked folder"
    Write-Host "[5] Memory-mapped file"
    Write-Host "[6] Reopen handle after Force Unlock"
    Write-Host "[7] Elevated/UAC lock (Retry Admin)"
    Write-Host "[8] Combined stress pack"
    Write-Host ""
    Write-Host "[S] Status"
    Write-Host "[O] Open Test Lab folder"
    Write-Host "[C] Cleanup current session"
    Write-Host "[Q] Quit + cleanup"
    Write-Host ""
}

try {
    while ($true) {
        Show-Menu
        $choice = Read-Host 'Select scenario'

        switch ($choice.ToUpperInvariant()) {
            '1' { Start-Scenario1; Read-Host 'Press Enter to return to menu' | Out-Null }
            '2' { Start-Scenario2; Read-Host 'Press Enter to return to menu' | Out-Null }
            '3' { Start-Scenario3; Read-Host 'Press Enter to return to menu' | Out-Null }
            '4' { Start-Scenario4; Read-Host 'Press Enter to return to menu' | Out-Null }
            '5' { Start-Scenario5; Read-Host 'Press Enter to return to menu' | Out-Null }
            '6' { Start-Scenario6; Read-Host 'Press Enter to return to menu' | Out-Null }
            '7' { Start-Scenario7; Read-Host 'Press Enter to return to menu' | Out-Null }
            '8' { Start-StressPack; Read-Host 'Press Enter to return to menu' | Out-Null }
            'S' { Show-Status; Read-Host 'Press Enter to return to menu' | Out-Null }
            'O' { Start-Process explorer.exe -ArgumentList ('"{0}"' -f $Root) }
            'C' { Clear-TestLab; Read-Host 'Press Enter to return to menu' | Out-Null }
            'Q' { break }
            default { }
        }

        if ($choice.ToUpperInvariant() -eq 'Q') { break }
    }
}
finally {
    Stop-TestWorkers -Quiet
    Start-Sleep -Milliseconds 250
    try {
        if (Test-Path -LiteralPath $Root) {
            Remove-Item -LiteralPath $Root -Recurse -Force -ErrorAction SilentlyContinue
        }
    } catch {}
}