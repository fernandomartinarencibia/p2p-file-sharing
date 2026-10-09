"""Prueba de extremo a extremo: servidor + dos clientes reales (client.py) dirigidos por stdin.

Uso (desde tests/): python3 test_e2e.py ./servidor_test ../client.py 45500 /tmp/p2p_e2e
El servicio SOAP de fecha y hora se sustituye por el módulo zeep falso de stub/.
"""
import os
import shutil
import subprocess
import sys
import time

SERVER, CLIENT, PORT, WORK = sys.argv[1], os.path.abspath(sys.argv[2]), sys.argv[3], sys.argv[4]
STUB = os.path.abspath(os.path.join(os.path.dirname(__file__), "stub"))

shutil.rmtree(WORK, ignore_errors=True)
dir_a, dir_b = os.path.join(WORK, "a"), os.path.join(WORK, "b")
os.makedirs(dir_a)
os.makedirs(dir_b)
open(os.path.join(dir_a, "hola.txt"), "w").write("contenido publicado\n")
open(os.path.join(dir_a, "secreto.txt"), "w").write("esto NO deberia salir\n")


def start_client(cwd):
    env = dict(os.environ, PYTHONPATH=STUB)
    return subprocess.Popen([sys.executable, "-u", CLIENT, "-s", "127.0.0.1", "-p", PORT],
                            cwd=cwd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, env=env)


def send(p, cmd, wait=0.6):
    p.stdin.write(cmd + "\n")
    p.stdin.flush()
    time.sleep(wait)


server = subprocess.Popen([SERVER, "-p", PORT], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1)

a = start_client(dir_a)
b = start_client(dir_b)
for cmd in ["REGISTER a", "CONNECT a", "PUBLISH hola.txt fichero de prueba"]:
    send(a, cmd)
for cmd in ["REGISTER b", "CONNECT b", "LIST_USERS", "LIST_CONTENT a",
            "GET_FILE a hola.txt copia.txt",
            "GET_FILE a secreto.txt robado.txt",
            "GET_FILE a /etc/hostname robado2.txt"]:
    send(b, cmd)

# reconexión de a en un proceso nuevo: sus ficheros siguen publicados en el servidor
send(a, "QUIT")
out_a1 = a.communicate(timeout=10)[0]
a2 = start_client(dir_a)
send(a2, "CONNECT a")
send(b, "GET_FILE a hola.txt copia_tras_reconexion.txt")
send(a2, "DELETE hola.txt")
send(b, "GET_FILE a hola.txt copia_tras_delete.txt")
send(a2, "QUIT")
send(b, "QUIT")
out_a2 = a2.communicate(timeout=10)[0]
out_b = b.communicate(timeout=10)[0]
server.terminate()
server.wait(timeout=5)

print("--- cliente a ---\n" + out_a1 + out_a2)
print("--- cliente b ---\n" + out_b)
print("--- ficheros descargados por b ---")
for f in sorted(os.listdir(dir_b)):
    print(f"  {f}: {open(os.path.join(dir_b, f)).read().strip()!r}")
