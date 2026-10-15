#!/usr/bin/env bash
# Downloads NVIDIA's DLSS Super Resolution model (nvngx_dlss.dll on Windows,
# libnvidia-ngx-dlss.so.<version> on Linux) from the public DLSS SDK (github.com/NVIDIA/DLSS),
# under NVIDIA's DLSS SDK license (LICENSE.txt there); it is not part of this repository.
# bbport loads it through the NVIDIA driver's NGX core when upscaler=dlss: it is searched next
# to the executable (out/ by default) and in BB_DLSS_DIR. Needs an NVIDIA RTX GPU.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
version=v310.9.1
base="https://raw.githubusercontent.com/NVIDIA/DLSS/$version/lib"
dest=${1:-out}
mkdir -p "$dest"
curl_client=curl
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        files=("Windows_x86_64/rel/nvngx_dlss.dll")
        if [[ -n ${WINDIR:-} ]] && command -v cygpath >/dev/null; then
            system_curl="$(cygpath -u "$WINDIR")/System32/curl.exe"
            if [[ -x $system_curl ]]; then curl_client=$system_curl; fi
        fi ;;
    *) files=("Linux_x86_64/rel/libnvidia-ngx-dlss.so.${version#v}") ;;
esac
"$curl_client" -fsSL --retry 3 --connect-timeout 20 --max-time 180 -o "$dest/LICENSE-DLSS-SDK.txt" "https://raw.githubusercontent.com/NVIDIA/DLSS/$version/LICENSE.txt"
for file in "${files[@]}"; do
    name=$(basename -- "$file")
    if [[ -s $dest/$name ]]; then echo "DLSS: $dest/$name present"; continue; fi
    "$curl_client" -fsSL --retry 3 --connect-timeout 20 --max-time 180 -o "$dest/$name.part" "$base/$file"
    mv "$dest/$name.part" "$dest/$name"
    echo "DLSS: downloaded $dest/$name"
done
