if defined SCOOP (set "SCOOP_DIR=%SCOOP%") else (set "SCOOP_DIR=%USERPROFILE%\scoop")

git clone --depth 1 --filter=blob:none --no-checkout https://github.com/aro-1790/hookshot-launchinst "%SCOOP_DIR%\buckets\hookshot-launchinst"
cd /D "%SCOOP_DIR%\buckets\hookshot-launchinst"
git sparse-checkout set --no-cone bucket/*
git checkout
