[CmdletBinding(DefaultParameterSetName = 'Definitions')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Sample')]
    [ValidateRange(1, 2147483647)]
    [int]$TargetProcessId,
    [Parameter(Mandatory, ParameterSetName = 'Sample')]
    [ValidateRange(1, 2147483647)]
    [int]$SupervisorProcessId,
    [Parameter(Mandatory, ParameterSetName = 'Sample')]
    [string]$ConfiguredInterpreter
)

function Write-NativeStartupObservation {
    param([hashtable]$Record)
    [Console]::Out.WriteLine(
        'native-python-startup-observation ' + ($Record | ConvertTo-Json -Depth 6 -Compress)
    )
    [Console]::Out.Flush()
}

function ConvertFrom-NativeStartupRecord {
    param([string]$Json)
    if ($Json.Length -gt 65536) {
        throw [ArgumentException]::new('A native startup record exceeds its size bound')
    }
    $record = ConvertFrom-Json -InputObject $Json -AsHashtable -NoEnumerate -Depth 8 -ErrorAction Stop
    if (-not ($record -is [Collections.IDictionary])) {
        throw [ArgumentException]::new('A native startup record is not an object')
    }
    $record
}

function Start-NativeStartupCapture {
    param([IO.StreamReader]$Reader)
    $buffer = [byte[]]::new(1048576)
    $storage = [IO.MemoryStream]::new($buffer, $true)
    @{
        Reader = $Reader
        Buffer = $buffer
        Storage = $storage
        Task = $Reader.BaseStream.CopyToAsync($storage, 4096)
    }
}

function Get-NativeStartupCaptureText {
    param([hashtable]$Capture)
    [void]$Capture.Task.GetAwaiter().GetResult()
    [Text.UTF8Encoding]::new($false, $true).GetString(
        $Capture.Buffer, 0, [int]$Capture.Storage.Position
    )
}

function New-NativeStartupObservation {
    param([Parameter(Mandatory)][string]$InterpreterPath)
    if (-not [IO.Path]::IsPathFullyQualified($InterpreterPath)) {
        throw 'The configured startup interpreter path is not absolute'
    }
    @{
        SupervisorPid = $PID
        InterpreterPath = [IO.Path]::GetFullPath($InterpreterPath)
        Script = Join-Path $PSScriptRoot 'native_python_startup_observer.ps1'
        OwnerPid = 0
        ReadinessObserved = $false
        Sampler = $null
        Output = $null
        ErrorOutput = $null
        Error = $null
        Closed = $false
    }
}

function Receive-NativeStartupProgress {
    param([hashtable]$Observation, [string]$Line)
    if ($Observation.Closed -or $Observation.ReadinessObserved -or $Observation.Error) {
        return
    }
    if ($Line -notmatch '^\s*\d+:\s+read-compatibility-progress (.*)$') {
        return
    }
    try {
        $record = ConvertFrom-NativeStartupRecord $Matches[1]
    } catch [ArgumentException] {
        $Observation.Error = 'Malformed native startup progress: ' + $_.Exception.Message
        return
    }
    if (-not $record.Contains('test') -or -not $record.Contains('phase') -or
        -not ($record.phase -is [string]) -or
        ($null -ne $record.test -and -not ($record.test -is [string]))) {
        $Observation.Error = 'Native startup progress has no module/test identity'
        return
    }
    if ($null -ne $record.test -or $record.phase -notin @('bootstrap-ready', 'module-ready')) {
        return
    }
    $ownerPid = 0
    if (-not [int]::TryParse([string]$record.pid, [ref]$ownerPid) -or $ownerPid -le 0) {
        $Observation.Error = 'Native startup progress has no valid process identity'
        return
    }
    if ($Observation.OwnerPid -ne 0 -and $Observation.OwnerPid -ne $ownerPid) {
        $Observation.Error = 'Native startup progress changed its process identity'
        return
    }
    if ($record.phase -eq 'module-ready') {
        if ($Observation.OwnerPid -eq 0) {
            $Observation.Error = 'Native startup readiness preceded its bootstrap record'
            return
        }
        $Observation.ReadinessObserved = $true
        if (-not $Observation.Sampler.HasExited) {
            $Observation.Sampler.StandardInput.Close()
        }
        return
    }
    if ($Observation.OwnerPid -ne 0) {
        return
    }
    $Observation.OwnerPid = $ownerPid
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = Join-Path $PSHOME 'pwsh.exe'
    $start.UseShellExecute = $false
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.UTF8Encoding]::new($false, $true)
    $start.StandardErrorEncoding = [Text.UTF8Encoding]::new($false, $true)
    foreach ($argument in @(
        '-NoLogo', '-NoProfile', '-NonInteractive', '-File', $Observation.Script,
        '-TargetProcessId', [string]$ownerPid,
        '-SupervisorProcessId', [string]$Observation.SupervisorPid,
        '-ConfiguredInterpreter', $Observation.InterpreterPath
    )) {
        $start.ArgumentList.Add($argument)
    }
    try {
        $Observation.Sampler = [Diagnostics.Process]::Start($start)
        $Observation.Output = Start-NativeStartupCapture $Observation.Sampler.StandardOutput
        $Observation.ErrorOutput = Start-NativeStartupCapture $Observation.Sampler.StandardError
    } catch [ComponentModel.Win32Exception], [InvalidOperationException] {
        $Observation.Error = 'Could not start the native startup sampler: ' + $_.Exception.Message
    }
}

function Complete-NativeStartupObservation {
    param([hashtable]$Observation)
    if ($Observation.Closed) {
        return
    }
    $Observation.Closed = $true
    $exited = $false
    $forced = $false
    $samplerPid = $null
    if ($null -ne $Observation.Sampler) {
        $sampler = $Observation.Sampler
        $samplerPid = $sampler.Id
        $cleanup = [Diagnostics.Stopwatch]::StartNew()
        try {
            if (-not $sampler.HasExited) {
                $sampler.StandardInput.Close()
            }
            if (-not $sampler.WaitForExit(1500)) {
                $sampler.Kill()
                $forced = $true
                $Observation.Error = 'The owned native startup sampler required termination'
                [void]$sampler.WaitForExit([Math]::Max(0, 2000 - [int]$cleanup.ElapsedMilliseconds))
            }
            $exited = $sampler.HasExited
            if (-not $exited) {
                $Observation.Error = 'The owned native startup sampler has not exited'
            }
            if ($exited -and $null -ne $Observation.Output -and
                $null -ne $Observation.ErrorOutput) {
                if (-not [Threading.Tasks.Task]::WaitAll(
                    [Threading.Tasks.Task[]]@($Observation.Output.Task, $Observation.ErrorOutput.Task),
                    [Math]::Max(0, 2000 - [int]$cleanup.ElapsedMilliseconds)
                )) {
                    $Observation.Error = 'The native startup sampler output did not close'
                } else {
                    $output = Get-NativeStartupCaptureText $Observation.Output
                    $errors = Get-NativeStartupCaptureText $Observation.ErrorOutput
                    if ($output.Length + $errors.Length -gt 2097152) {
                        $Observation.Error = 'The native startup sampler exceeded its output bound'
                    } else {
                        [Console]::Out.Write($output)
                        [Console]::Error.Write($errors)
                    }
                    if ($sampler.ExitCode -ne 0) {
                        $Observation.Error = "The native startup sampler failed with exit $($sampler.ExitCode)"
                    } elseif (-not $Observation.Error) {
                        $lastRecord = $null
                        foreach ($line in $output.Split("`n", [StringSplitOptions]::RemoveEmptyEntries)) {
                            if (-not $line.StartsWith('native-python-startup-observation ')) {
                                $Observation.Error = 'The native startup sampler emitted unexpected output'
                                break
                            }
                            try {
                                $lastRecord = ConvertFrom-NativeStartupRecord (
                                    $line.Substring('native-python-startup-observation '.Length).TrimEnd("`r")
                                )
                            } catch [ArgumentException] {
                                $Observation.Error = 'Malformed native sampler output: ' + $_.Exception.Message
                                break
                            }
                            if ($lastRecord.owner_pid -ne $Observation.OwnerPid) {
                                $Observation.Error = 'The native startup sampler changed its target identity'
                                break
                            }
                        }
                        if (-not $Observation.Error -and
                            ($null -eq $lastRecord -or
                             $lastRecord.phase -notin @('sampler-finished', 'target-unavailable'))) {
                            $Observation.Error = 'The native startup sampler emitted no complete observation'
                        }
                    }
                }
            }
        } catch [AggregateException], [IO.IOException], [InvalidOperationException], [ComponentModel.Win32Exception], [Text.DecoderFallbackException] {
            $Observation.Error = 'Native startup sampler cleanup failed: ' + $_.Exception.Message
            foreach ($capture in @($Observation.Output, $Observation.ErrorOutput)) {
                if ($null -ne $capture -and $capture.Task.IsFaulted) {
                    foreach ($failure in $capture.Task.Exception.Flatten().InnerExceptions) {
                        if ($failure -is [NotSupportedException]) {
                            $Observation.Error = 'The native startup sampler exceeded its output bound while collecting'
                        }
                    }
                }
            }
            if (-not $sampler.HasExited) {
                $sampler.Kill()
                $forced = $true
                [void]$sampler.WaitForExit([Math]::Max(0, 2000 - [int]$cleanup.ElapsedMilliseconds))
            }
            $exited = $sampler.HasExited
        } finally {
            foreach ($capture in @($Observation.Output, $Observation.ErrorOutput)) {
                if ($null -ne $capture) {
                    $capture.Reader.Dispose()
                    $capture.Storage.Dispose()
                }
            }
            $sampler.Dispose()
        }
    }
    Write-NativeStartupObservation @{
        phase = 'observation-complete'
        owner_pid = $Observation.OwnerPid
        sampler_pid = $samplerPid
        readiness_observed = $Observation.ReadinessObserved
        sampler_exited = $exited
        forced_termination = $forced
        error = $Observation.Error
    }
}

function Invoke-NativeStartupSampler {
    param([int]$OwnerPid, [int]$SupervisorPid, [string]$InterpreterPath)
    if (-not [IO.Path]::IsPathFullyQualified($InterpreterPath)) {
        throw 'The configured startup interpreter path is not absolute'
    }
    $interpreter = [IO.Path]::GetFullPath($InterpreterPath)
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $inputReader = [IO.StreamReader]::new([Console]::OpenStandardInput())
    $stop = $inputReader.ReadLineAsync()
    $process = $null
    $supervisor = $null
    try {
        $properties = @('ProcessId', 'ParentProcessId', 'CreationDate', 'Name', 'ExecutablePath')
        $owner = Get-CimInstance Win32_Process -Filter "ProcessId = $OwnerPid" `
            -Property $properties -OperationTimeoutSec 1 -ErrorAction Stop
        if ($null -eq $owner) {
            Write-NativeStartupObservation @{
                phase = 'target-unavailable'
                owner_pid = $OwnerPid
                reason = 'No live target remained when identity was queried'
            }
            return
        }
        $unavailable = $false
        try {
            $process = [Diagnostics.Process]::GetProcessById($OwnerPid)
            $supervisor = [Diagnostics.Process]::GetProcessById($SupervisorPid)
            $created = $process.StartTime.ToUniversalTime().Ticks
            $supervisorCreated = $supervisor.StartTime.ToUniversalTime().Ticks
        } catch [ArgumentException] {
            $unavailable = $true
        } catch [InvalidOperationException], [ComponentModel.Win32Exception] {
            $unavailable = ($null -ne $process -and $process.HasExited) -or
                ($null -ne $supervisor -and $supervisor.HasExited)
            if (-not $unavailable) {
                throw
            }
        }
        if ($unavailable) {
            Write-NativeStartupObservation @{
                phase = 'target-unavailable'
                owner_pid = $OwnerPid
                reason = 'The observed process chain exited before metadata was opened'
            }
            return
        }
        $parent = Get-CimInstance Win32_Process -Filter "ProcessId = $($owner.ParentProcessId)" `
            -Property $properties -OperationTimeoutSec 1 -ErrorAction Stop
        if ([string]::IsNullOrWhiteSpace($owner.ExecutablePath)) {
            if (-not $process.HasExited) {
                throw 'The live startup target has no observable executable identity'
            }
            Write-NativeStartupObservation @{
                phase = 'target-unavailable'
                owner_pid = $OwnerPid
                reason = 'The target exited before its executable identity was available'
            }
            return
        }
        $configured = $owner
        $ctest = $parent
        $redirected = $false
        if (-not [string]::Equals(
            [IO.Path]::GetFullPath($owner.ExecutablePath), $interpreter,
            [StringComparison]::OrdinalIgnoreCase
        )) {
            if ($null -eq $parent -or [string]::IsNullOrWhiteSpace($parent.ExecutablePath)) {
                if (-not $process.HasExited) {
                    throw 'The live startup target has no configured launcher identity'
                }
                Write-NativeStartupObservation @{
                    phase = 'target-unavailable'
                    owner_pid = $OwnerPid
                    reason = 'The target exited before its launcher identity was available'
                }
                return
            }
            if (-not [string]::Equals(
                [IO.Path]::GetFullPath($parent.ExecutablePath), $interpreter,
                [StringComparison]::OrdinalIgnoreCase
            )) {
                throw 'The startup target does not belong to the configured interpreter'
            }
            $configured = $parent
            $redirected = $true
            $ctest = Get-CimInstance Win32_Process -Filter "ProcessId = $($configured.ParentProcessId)" `
                -Property $properties -OperationTimeoutSec 1 -ErrorAction Stop
        }
        $root = Get-CimInstance Win32_Process -Filter "ProcessId = $SupervisorPid" `
            -Property $properties -OperationTimeoutSec 1 -ErrorAction Stop
        if (($null -eq $ctest -or $null -eq $root) -and
            ($process.HasExited -or $supervisor.HasExited)) {
            Write-NativeStartupObservation @{
                phase = 'target-unavailable'
                owner_pid = $OwnerPid
                reason = 'The process chain exited during identity observation'
            }
            return
        }
        if ($null -eq $ctest -or
            $ctest.Name -ine 'ctest.exe' -or $ctest.ParentProcessId -ne $SupervisorPid -or
            $null -eq $root -or $root.Name -ine 'pwsh.exe') {
            throw 'The startup target is not an owned Python/CTest/workflow-shell process chain'
        }
        if ($null -eq $owner.CreationDate -or $null -eq $configured.CreationDate -or
            $null -eq $ctest.CreationDate -or
            $null -eq $root.CreationDate -or
            $root.CreationDate -gt $ctest.CreationDate -or
            $ctest.CreationDate -gt $configured.CreationDate -or
            $configured.CreationDate -gt $owner.CreationDate) {
            throw 'The startup process-chain creation identities are inconsistent'
        }
        $cimCreated = $owner.CreationDate.ToUniversalTime().Ticks
        if ($created - ($created % 10) -ne $cimCreated - ($cimCreated % 10)) {
            throw 'The startup target changed its creation identity'
        }
        Write-NativeStartupObservation @{
            phase = 'target'
            owner_pid = $OwnerPid
            owner_image = $owner.ExecutablePath
            configured_interpreter = $interpreter
            configured_pid = [int]$configured.ProcessId
            configured_created_utc_ticks = $configured.CreationDate.ToUniversalTime().Ticks
            redirected = $redirected
            ctest_pid = [int]$ctest.ProcessId
            supervisor_pid = $SupervisorPid
            owner_created_utc_ticks = $created
            ctest_created_utc_ticks = $ctest.CreationDate.ToUniversalTime().Ticks
            supervisor_created_utc_ticks = $supervisorCreated
            monotonic_frequency = [Diagnostics.Stopwatch]::Frequency
        }
        $count = 0
        $reason = 'sample-window-ended'
        while ($clock.Elapsed.TotalSeconds -lt 10 -and $count -lt 40) {
            if ($stop.IsCompleted) {
                if ($null -ne $stop.GetAwaiter().GetResult()) {
                    throw 'The startup sampler received an unexpected control message'
                }
                $reason = 'controller-stopped'
                break
            }
            if ($process.HasExited -or $supervisor.HasExited) {
                $reason = 'process-exited'
                break
            }
            $before = $clock.ElapsedTicks
            $unix = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
            try {
                $process.Refresh()
                $supervisor.Refresh()
                if ($process.StartTime.ToUniversalTime().Ticks -ne $created -or
                    $supervisor.StartTime.ToUniversalTime().Ticks -ne $supervisorCreated) {
                    throw 'A sampled process changed its creation identity'
                }
                $cpu = $process.TotalProcessorTime.Ticks
                $threads = @($process.Threads)
                if ($threads.Count -eq 0 -or $threads.Count -gt 128) {
                    throw 'The startup target has an invalid or oversized thread population'
                }
                $observed = foreach ($thread in $threads) {
                    $state = $thread.ThreadState.ToString()
                    $waitReason = $null
                    if ($thread.ThreadState -eq [Diagnostics.ThreadState]::Wait) {
                        $waitReason = $thread.WaitReason.ToString()
                    }
                    try {
                        $threadCpu = $thread.TotalProcessorTime.Ticks
                        @{ id = $thread.Id; state = $state; wait_reason = $waitReason; cpu_ticks = $threadCpu }
                    } catch [ComponentModel.Win32Exception] {
                        if ($_.Exception.NativeErrorCode -notin @(87, 1168)) {
                            throw
                        }
                        @{
                            id = $thread.Id; state = 'unavailable'; wait_reason = $null
                            cpu_ticks = $null; error = 'thread-exited-during-sample'
                        }
                    } finally {
                        $thread.Dispose()
                    }
                }
            } catch [InvalidOperationException], [ComponentModel.Win32Exception] {
                if (-not $process.HasExited -and -not $supervisor.HasExited) {
                    throw
                }
                $reason = 'process-exited-during-sample'
                break
            }
            Write-NativeStartupObservation @{
                phase = 'sample'
                owner_pid = $OwnerPid
                unix_ms = $unix
                elapsed_ticks = $before
                observation_ticks = $clock.ElapsedTicks - $before
                process_cpu_ticks = $cpu
                threads = @($observed)
            }
            $count++
            [void]$stop.Wait(250)
        }
        Write-NativeStartupObservation @{
            phase = 'sampler-finished'
            owner_pid = $OwnerPid
            samples = $count
            reason = $reason
            elapsed_ticks = $clock.ElapsedTicks
        }
    } finally {
        if ($null -ne $process) { $process.Dispose() }
        if ($null -ne $supervisor) { $supervisor.Dispose() }
        $inputReader.Dispose()
    }
}

if ($PSCmdlet.ParameterSetName -eq 'Sample') {
    $ErrorActionPreference = 'Stop'
    Invoke-NativeStartupSampler -OwnerPid $TargetProcessId -SupervisorPid $SupervisorProcessId `
        -InterpreterPath $ConfiguredInterpreter
}
