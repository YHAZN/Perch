$pythonPath = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\python.exe'
& $pythonPath (Join-Path $PSScriptRoot 'server.py')
