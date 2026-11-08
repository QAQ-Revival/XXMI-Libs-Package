param(
    [string]$WorkRoot,
    [switch]$Test,
    [string]$EfmiShader
)
$ErrorActionPreference = 'Stop'
$taskSource = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if (!$WorkRoot) { $WorkRoot = Join-Path $taskSource 'build/qaqr' }
$taskWork = [IO.Path]::GetFullPath($WorkRoot)
foreach ($taskRoot in @($taskSource, $taskWork)) {
    for ($taskParent = $taskRoot; $taskParent; $taskParent = Split-Path -Parent $taskParent) {
        if ((Split-Path -Leaf $taskParent) -in @('Mods','Core','ShaderFixes','ShaderCache') -or (Test-Path -LiteralPath (Join-Path $taskParent 'd3dx.ini'))) { throw 'Build outside a live importer directory' }
    }
}
$taskVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$taskVisualStudio = & $taskVswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$taskVisualStudio) { throw 'Visual C++ x64 build tools are required' }
$taskSdk = Get-ChildItem -LiteralPath (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/Include') -Directory | Where-Object Name -Match '^10\.0\.\d+\.0$' | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1 -ExpandProperty Name
if (!$taskSdk) { throw 'Windows 10 SDK is required' }
New-Item -ItemType Directory -Path $taskWork -Force | Out-Null
$taskEnvironment = @{}
foreach ($name in @('GITHUB_ACTOR','GIT_AUTHOR_NAME','GIT_AUTHOR_EMAIL','GIT_COMMITTER_NAME','GIT_COMMITTER_EMAIL','CL')) { $taskEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    $env:GITHUB_ACTOR = 'qaq-revival-publisher[bot]'
    $env:GIT_AUTHOR_NAME = $env:GITHUB_ACTOR
    $env:GIT_COMMITTER_NAME = $env:GITHUB_ACTOR
    $env:GIT_AUTHOR_EMAIL = '338412940+qaq-revival-publisher[bot]@users.noreply.github.com'
    $env:GIT_COMMITTER_EMAIL = $env:GIT_AUTHOR_EMAIL
    $env:CL = ($env:CL + ' "/pathmap:' + $taskSource + '=/src" "/pathmap:' + $taskWork + '=/build"').Trim()
    Push-Location $taskSource
    try {
        $taskGenerated = Join-Path $taskWork 'version.generated.h'
        & ./tools/generate-version.ps1 -VersionFile (Join-Path $taskSource 'version.h') -OutputFile $taskGenerated
        if ($LASTEXITCODE -ne 0) { throw 'Version resource generation failed' }
        $taskHeader = Join-Path $taskSource 'version.generated.h'
        if (!(Test-Path -LiteralPath $taskHeader) -or [IO.File]::ReadAllText($taskGenerated) -ne [IO.File]::ReadAllText($taskHeader)) { Copy-Item -LiteralPath $taskGenerated -Destination $taskHeader }
        & (Join-Path $taskVisualStudio 'MSBuild/Current/Bin/MSBuild.exe') ./StereovisionHacks.sln /t:DirectX11 '/p:Configuration=Zip Release' /p:Platform=x64 "/p:WindowsTargetPlatformVersion=$taskSdk" /m:4 /v:minimal /nologo
        if ($LASTEXITCODE -ne 0) { throw 'Native build failed' }
    } finally { Pop-Location }
    Copy-Item -LiteralPath (Join-Path $taskSource 'x64/Zip Release/d3d11.dll') -Destination (Join-Path $taskWork 'd3d11.dll')
    if ($Test) {
        Import-Module (Join-Path $taskVisualStudio 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
        Enter-VsDevShell -VsInstallPath $taskVisualStudio -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
        if (!$EfmiShader) { $EfmiShader = Join-Path $PSScriptRoot 'tests/instance-config.hlsl' }
        $taskShader = (Resolve-Path -LiteralPath $EfmiShader).Path
        Push-Location $taskWork
        try {
            & cl.exe /nologo /EHsc /std:c++17 /O2 /MT (Join-Path $PSScriptRoot 'tests/autosave.cpp') /Fe:autosave.exe /link d3dcompiler.lib user32.lib
            if ($LASTEXITCODE -ne 0) { throw 'Autosave probe build failed' }
            & cl.exe /nologo /EHsc /std:c++17 /O2 /MT (Join-Path $PSScriptRoot 'tests/region.cpp') /Fe:region.exe /link d3dcompiler.lib
            if ($LASTEXITCODE -ne 0) { throw 'Region probe build failed' }
            & cl.exe /nologo /EHsc /std:c++17 /O2 /MT (Join-Path $PSScriptRoot 'tests/share.cpp') /Fe:share.exe /link d3dcompiler.lib
            if ($LASTEXITCODE -ne 0) { throw 'Share probe build failed' }
            & cl.exe /nologo /EHsc /std:c++17 /O2 /MT (Join-Path $PSScriptRoot 'tests/initext.cpp') /Fe:initext.exe
            if ($LASTEXITCODE -ne 0) { throw 'Ini text probe build failed' }
            & cl.exe /nologo /EHsc /std:c++17 /O2 /MT (Join-Path $PSScriptRoot 'tests/reload.cpp') /Fe:reload.exe /link d3dcompiler.lib user32.lib
            if ($LASTEXITCODE -ne 0) { throw 'Reload probe build failed' }
            & ./initext.exe
            if ($LASTEXITCODE -ne 0) { throw 'Ini text decoding differs from the stream it replaced' }
            $taskDll = Join-Path $taskWork 'd3d11.dll'
            $taskFixture = Join-Path $taskWork ('autosave-' + [guid]::NewGuid().ToString('N'))
            & ./autosave.exe $taskDll $taskFixture initial
            if ($LASTEXITCODE -ne 0) { throw 'Incremental save validation failed' }
            & ./autosave.exe $taskDll $taskFixture restore
            if ($LASTEXITCODE -ne 0) { throw 'Restore validation failed' }
            foreach ($case in 0..14) {
                & ./region.exe $taskDll patched $case $taskShader
                if ($LASTEXITCODE -ne 0) { throw "COPY/REF regression failed: $case" }
            }
            foreach ($mode in @('on','off')) {
                & ./share.exe $taskDll $mode
                if ($LASTEXITCODE -ne 0) { throw "Duplicate resource sharing failed: $mode" }
                & ./reload.exe $taskDll $mode
                if ($LASTEXITCODE -ne 0) { throw "Config reload reuse failed: $mode" }
            }
        } finally { Pop-Location }
    }
    $taskHash = [Security.Cryptography.SHA256]::Create()
    $taskStream = [IO.File]::OpenRead((Join-Path $taskWork 'd3d11.dll'))
    try { Write-Output ('SHA256 ' + [BitConverter]::ToString($taskHash.ComputeHash($taskStream)).Replace('-', '').ToLowerInvariant()) }
    finally { $taskStream.Dispose(); $taskHash.Dispose() }
} finally {
    foreach ($entry in $taskEnvironment.GetEnumerator()) { [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process') }
}
