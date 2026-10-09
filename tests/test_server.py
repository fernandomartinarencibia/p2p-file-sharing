"""Banco de pruebas del servidor central: habla su protocolo directamente por TCP.

Uso (desde tests/): python3 test_server.py ./servidor_asan 45001 <prueba>
Pruebas: functional | register_race | list_content_race
"""
import os
import socket
import subprocess
import sys
import threading
import time

BIN, PORT, TEST = sys.argv[1], int(sys.argv[2]), sys.argv[3]
DATE = "09/10/2026 02:50:00"


def recv_str(s):
    out = b""
    while True:
        c = s.recv(1)
        if not c or c == b"\0":
            return out.decode()
        out += c


def call(op, *fields, extra=None):
    """Envía una operación y devuelve (código, datos adicionales)."""
    with socket.create_connection(("127.0.0.1", PORT), timeout=10) as s:
        s.sendall(op.encode() + b"\0" + DATE.encode() + b"\0")
        for f in fields:
            s.sendall(str(f).encode() + b"\0")
        code = s.recv(1).decode()
        data = None
        if code == "0" and op == "LIST_USERS":
            n = int(recv_str(s))
            data = [(recv_str(s), recv_str(s), recv_str(s)) for _ in range(n)]
        elif code == "0" and op == "LIST_CONTENT":
            n = int(recv_str(s))
            data = [recv_str(s) for _ in range(n)]
        return code, data


def start_server(log):
    env = dict(os.environ, ASAN_OPTIONS="abort_on_error=0:halt_on_error=1",
               TSAN_OPTIONS="halt_on_error=0:report_signal_unsafe=0")
    env.pop("LOG_RPC_IP", None)
    p = subprocess.Popen([BIN, "-p", str(PORT)], stdout=subprocess.DEVNULL, stderr=log, env=env)
    for _ in range(50):
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.1)
    # la conexión de sondeo deja un hilo esperando datos: cerrarla provoca un error de lectura inocuo
    return p


def check(label, got, expected):
    status = "OK " if got == expected else "FALLO"
    print(f"  [{status}] {label}: obtenido={got!r} esperado={expected!r}")
    return got == expected


def functional():
    ok = True
    ok &= check("REGISTER a", call("REGISTER", "a")[0], "0")
    ok &= check("REGISTER a repetido", call("REGISTER", "a")[0], "1")
    ok &= check("REGISTER b", call("REGISTER", "b")[0], "0")
    ok &= check("PUBLISH sin conectar", call("PUBLISH", "a", "f1", "desc")[0], "2")
    ok &= check("CONNECT a", call("CONNECT", "a", 40001)[0], "0")
    ok &= check("CONNECT a repetido", call("CONNECT", "a", 40001)[0], "2")
    ok &= check("CONNECT inexistente", call("CONNECT", "zz", 40002)[0], "1")
    ok &= check("PUBLISH f1", call("PUBLISH", "a", "f1", "desc uno")[0], "0")
    ok &= check("PUBLISH f2", call("PUBLISH", "a", "f2", "desc dos")[0], "0")
    ok &= check("PUBLISH f1 repetido", call("PUBLISH", "a", "f1", "x")[0], "3")
    code, files = call("LIST_CONTENT", "a", "a")
    ok &= check("LIST_CONTENT a", (code, sorted(files or [])), ("0", ["f1", "f2"]))
    ok &= check("LIST_CONTENT de inexistente", call("LIST_CONTENT", "a", "zz")[0], "3")
    ok &= check("LIST_CONTENT desde no conectado", call("LIST_CONTENT", "b", "a")[0], "2")
    ok &= check("LIST_CONTENT desde inexistente", call("LIST_CONTENT", "zz", "a")[0], "1")
    code, users = call("LIST_USERS", "a")
    ok &= check("LIST_USERS", (code, users), ("0", [("a", "127.0.0.1", "40001")]))
    ok &= check("LIST_USERS desde no conectado", call("LIST_USERS", "b")[0], "2")
    ok &= check("DELETE f1", call("DELETE", "a", "f1")[0], "0")
    ok &= check("DELETE f1 repetido", call("DELETE", "a", "f1")[0], "3")
    code, files = call("LIST_CONTENT", "a", "a")
    ok &= check("LIST_CONTENT tras DELETE", (code, files), ("0", ["f2"]))
    ok &= check("DISCONNECT a", call("DISCONNECT", "a")[0], "0")
    ok &= check("DISCONNECT a repetido", call("DISCONNECT", "a")[0], "2")
    ok &= check("UNREGISTER a", call("UNREGISTER", "a")[0], "0")
    ok &= check("UNREGISTER a repetido", call("UNREGISTER", "a")[0], "1")
    ok &= check("UNREGISTER b", call("UNREGISTER", "b")[0], "0")
    return ok


def register_race(rounds=200, threads=16):
    """Registros simultáneos del mismo nombre: solo uno debería tener éxito."""
    duplicated_rounds = 0
    for r in range(rounds):
        name = f"u{r}"
        barrier = threading.Barrier(threads)
        results = []

        def worker():
            barrier.wait()
            results.append(call("REGISTER", name)[0])

        ts = [threading.Thread(target=worker) for _ in range(threads)]
        [t.start() for t in ts]
        [t.join() for t in ts]
        if results.count("0") > 1:
            duplicated_rounds += 1
    print(f"  Rondas con el mismo usuario registrado más de una vez: {duplicated_rounds}/{rounds}")
    return duplicated_rounds == 0


def list_content_race(seconds=8):
    """LIST_CONTENT sobre un usuario mientras otro hilo lo da de baja y lo vuelve a crear."""
    call("REGISTER", "reader")
    call("CONNECT", "reader", 40010)
    stop = time.time() + seconds
    errors = []

    def churn():
        while time.time() < stop:
            call("REGISTER", "target")
            call("CONNECT", "target", 40011)
            for i in range(300):
                call("PUBLISH", "target", f"file_{i:03d}", "d")
            call("UNREGISTER", "target")

    def reader():
        while time.time() < stop:
            try:
                call("LIST_CONTENT", "reader", "target")
            except Exception as e:  # el servidor puede haber caído
                errors.append(repr(e))
                return

    ts = [threading.Thread(target=churn)] + [threading.Thread(target=reader) for _ in range(6)]
    [t.start() for t in ts]
    [t.join() for t in ts]
    return errors


if __name__ == "__main__":
    log_path = f"server_{TEST}_{os.path.basename(BIN)}.log"
    with open(log_path, "w") as log:
        srv = start_server(log)
        try:
            if TEST == "functional":
                result = functional()
            elif TEST == "register_race":
                result = register_race()
            else:
                errs = list_content_race()
                result = not errs
                if errs:
                    print("  Errores de conexión (el servidor cayó):", errs[:2])
        finally:
            time.sleep(0.5)
            alive = srv.poll() is None
            srv.terminate()
            srv.wait(timeout=5)
    report = open(log_path).read()
    for marker in ["heap-use-after-free", "ERROR: AddressSanitizer", "WARNING: ThreadSanitizer: data race",
                   "WARNING: ThreadSanitizer: heap-use-after-free"]:
        n = report.count(marker)
        if n:
            print(f"  Informe del sanitizador: '{marker}' x{n}")
            result = False
    print(f"  Servidor vivo al terminar: {alive}")
    print("RESULTADO:", "PASA" if result else "FALLA")
