# a wine to run the sdk's ShaderCompile2.exe (windows only) on linux: tools/build/shaders_linux.py runs it with no
# network and only the shader folders mounted. microsoft's d3dcompiler_47.dll goes next to the exe (wine's own
# compiler isn't fxc's byte for byte)
FROM debian:trixie-slim
RUN dpkg --add-architecture i386 && apt-get update \
 && apt-get install -y --no-install-recommends wine wine64 ca-certificates \
 && rm -rf /var/lib/apt/lists/*
ENV WINEDEBUG=-all WINEDLLOVERRIDES="d3dcompiler_47=n;mscoree,mshtml=" WINEPREFIX=/wine
RUN wineboot --init 2>/dev/null; wineserver -w; chmod -R a+rwX /wine
