$ErrorActionPreference = 'Stop'

cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build ./build --config Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& ".\build\Source\Dexed_artefacts\Release\Standalone\Dexed.exe"
