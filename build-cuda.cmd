rd /s build

call "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvarsall.bat" amd64
meson.py build --backend vs2017 --buildtype release ^
-Dcudnn_libdirs="C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v9.2\lib\x64","C:\dev\cuDNN\cuda\lib\x64" ^
-Dcudnn_include="C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v9.2\include" ^
-Ddefault_library=static

cd build

"C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\MSBuild\15.0\Bin\MSBuild.exe" ^
/p:Configuration=Release ^
/p:Platform=x64 ^
/p:PreferredToolArchitecture=x64 lc0.sln ^
/filelogger