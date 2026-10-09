#!/usr/bin/env bash
# Compila el servidor con AddressSanitizer y ThreadSanitizer (sustituyendo el logger RPC por un stub)
# y ejecuta las pruebas funcionales, de concurrencia y de extremo a extremo.
set -euo pipefail
cd "$(dirname "$0")"
BUILD=$(mktemp -d)
cp ../server.c "$BUILD/"   # se compila en un directorio aparte para que se use stub/logger.h
for san in address thread; do
    gcc -Wall -Wextra -Wno-unused-parameter -g -O1 -fsanitize=$san -I stub \
        -o "servidor_$san" "$BUILD/server.c" -pthread
done
gcc -Wall -g -I stub -o servidor_test "$BUILD/server.c" -pthread
rm -rf "$BUILD"

port=46000
for san in address thread; do
    for t in functional register_race list_content_race; do
        port=$((port + 1))
        echo "=== [$san] $t"
        python3 test_server.py "./servidor_$san" "$port" "$t" 2>/dev/null | grep -E "FALLO|Rondas|sanitizador|RESULTADO"
    done
done

echo "=== extremo a extremo"
python3 test_e2e.py ./servidor_test ../client.py $((port + 1)) "$(mktemp -d)/e2e" | sed -n '/cliente b/,$p'
