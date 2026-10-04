#!/usr/bin/env bash
# Usage: ./verify_wav.sh [ogg-only]
# Place in the soloud repo root. Env overrides: CXX, CC.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
CXX="${CXX:-g++}"
CC="${CC:-gcc}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

DEFS=(-DWITH_MINIAUDIO)
if [[ "${1:-}" == "ogg-only" ]]; then
    DEFS+=(-DSOLOUD_OGG_ONLY=1)
    echo "Build: miniaudio, SOLOUD_OGG_ONLY=1"
else
    echo "Build: miniaudio, all formats"
fi

case "$(uname -s)" in
    Darwin) LIBS=(-framework CoreFoundation -framework CoreAudio -framework AudioToolbox) ;;
    *)      LIBS=(-ldl -lpthread -lm) ;;
esac

cat > "$OUT/main.cpp" <<'EOF'
#include <stdio.h>
#include <string.h>
#include "soloud.h"
#include "soloud_thread.h"
#include "soloud_wav.h"
#include "soloud_wavstream.h"

static void play(SoLoud::Soloud &aSoloud, SoLoud::AudioSource &aSrc, const char *aName, double aLength){
    printf("%s: length %.3f s, playing...\n", aName, aLength);
    aSoloud.play(aSrc);
    while(aSoloud.getActiveVoiceCount() > 0)
        SoLoud::Thread::sleep(100);
}

int main(){
    char path[1024];
    printf("Sound path: ");
    fflush(stdout);
    if(!fgets(path, sizeof(path), stdin))
        return 1;
    path[strcspn(path, "\r\n")] = 0;

    SoLoud::Soloud soloud;
    SoLoud::result res = soloud.init(SoLoud::Soloud::CLIP_ROUNDOFF, SoLoud::Soloud::MINIAUDIO);
    if(res != SoLoud::SO_NO_ERROR){
        printf("Soloud init failed: %d\n", (int)res);
        return 1;
    }

    int rc = 0;

    SoLoud::Wav wav;
    res = wav.load(path);
    if(res == SoLoud::SO_NO_ERROR)
        play(soloud, wav, "Wav", wav.getLength());
    else{
        printf("Wav load failed: %d\n", (int)res);
        rc = 1;
    }

    SoLoud::WavStream stream;
    res = stream.load(path);
    if(res == SoLoud::SO_NO_ERROR)
        play(soloud, stream, "WavStream", stream.getLength());
    else{
        printf("WavStream load failed: %d\n", (int)res);
        rc = 1;
    }

    soloud.deinit();
    return rc;
}
EOF

echo "Compiling..."
"$CC" -c -O2 "${DEFS[@]}" -I"$ROOT/include" -I"$ROOT/src/audiosource/wav" \
    "$ROOT/src/audiosource/wav/stb_vorbis.c" -o "$OUT/stb_vorbis.o"

"$CXX" -O2 "${DEFS[@]}" \
    -I"$ROOT/include" -I"$ROOT/src/audiosource/wav" -I"$ROOT/src/backend/miniaudio" \
    "$OUT/main.cpp" \
    "$ROOT"/src/core/*.cpp \
    "$ROOT"/src/filter/*.cpp \
    "$ROOT"/src/audiosource/wav/soloud_wav.cpp \
    "$ROOT"/src/audiosource/wav/soloud_wavstream.cpp \
    "$ROOT"/src/audiosource/wav/dr_impl.cpp \
    "$ROOT"/src/backend/miniaudio/soloud_miniaudio.cpp \
    "$OUT/stb_vorbis.o" \
    "${LIBS[@]}" -o "$OUT/verify_wav"

echo "Running..."
"$OUT/verify_wav"