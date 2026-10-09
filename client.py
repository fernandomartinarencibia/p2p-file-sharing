from enum import Enum

import argparse
import socket
import threading
from zeep import Client #para el web service de obetener hora

MAX_CONNECTION_QUEUE = 5
MAX_USER_NAME_LENGTH = 256
MAX_FILE_NAME_LENGTH = 256
MAX_FILE_DESC_LENGTH = 256


class client :



    # ******************** TYPES *********************

    # *

    # * @brief Return codes for the protocol methods

    class RC(Enum) :

        OK = 0

        ERROR = 1

        USER_ERROR = 2



    # ****************** ATTRIBUTES ******************

    _server = None

    _port = -1

    _user_name_after_connect = None
    is_connected = False
    _listen_sock = None
    _hilo_servicio = None

    _ws_client = None  # cliente SOAP de fecha/hora

    # ficheros publicados por el usuario conectado: el servicio de descargas solo sirve estos,
    # para que otros clientes no puedan pedir cualquier ruta del sistema de ficheros
    _published_files = set()



    # ******************** METHODS *******************

    @staticmethod
    def send_fecha_hora(sock):
        fecha_hora = client._ws_client.service.obtener_fecha_hora()
        sock.sendall(fecha_hora.encode('utf-8') + b"\0")

    @staticmethod

    def  register(user) :
        #comprobacion de longitud correcta
        if (len(user) > MAX_USER_NAME_LENGTH - 1):
            print(f"REGISTER FAIL")
            return client.RC.USER_ERROR
        
        try:
            #crear socket tcp
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
                #conectar con el servidor
                sock.connect((client._server, client._port))

                #enviar operacion "REGISTER" + \0
                sock.sendall(b"REGISTER\0")

                # obtener la fecha y hora del webservice y enviarla
                client.send_fecha_hora(sock)

                #enviar el user_name
                sock.sendall(user.encode('utf-8') + b"\0")

                #recibir respuesta del server
                respuesta = sock.recv(1)

                #decodificar bytes de la respuesta
                respuesta = respuesta.decode()

                if respuesta == '0':
                    print(f"REGISTER OK")
                    return client.RC.OK
                elif respuesta == '1':
                    print(f"USERNAME IN USE")
                    return client.RC.USER_ERROR
                else:
                    print(f"REGISTER FAIL")
                    return client.RC.ERROR

        except Exception:
            print(f"REGISTER FAIL")
            return client.RC.ERROR

    @staticmethod

    def  unregister(user) :
        if (len(user) > MAX_USER_NAME_LENGTH - 1):
            print(f"UNREGISTER FAIL")
            return client.RC.ERROR
        
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
                #conectar al server
                sock.connect((client._server,client._port))

                #envio de operacion
                sock.sendall(b"UNREGISTER\0")

                # obtener la fecha y hora del webservice y enviarla
                client.send_fecha_hora(sock)

                #envio de user_name
                sock.sendall(user.encode('utf-8') + b"\0")

                #recibir respuesta del server
                respuesta = sock.recv(1)
                #decodificar respuesta
                respuesta = respuesta.decode()

                if (respuesta == '0'):
                    #unregister correcto
                    print(f"UNREGISTER OK")
                    return client.RC.OK
                elif (respuesta == '1'):
                    #no existe usuario
                    print(f"USER DOES NOT EXIST")
                    return client.RC.USER_ERROR
                else:
                    #cualquier otro error
                    print(f"UNREGISTER FAIL")
                    return client.RC.ERROR
                
        except Exception:
            print(f"UNREGISTER FAIL")
            return client.RC.ERROR

    @staticmethod

    def  connect(user) :
        if ((len(user) > MAX_USER_NAME_LENGTH - 1)):
            print(f"CONNECT FAIL")
            return client.RC.ERROR
        
        if client.is_connected:
            if client._user_name_after_connect == user:
                print("USER ALREADY CONNECTED")
                return client.RC.USER_ERROR
            else:
                client.disconnect(client._user_name_after_connect)
        
        try:
            #primero crear socket de servicio para descargas de otros clientes
            listen_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            listen_sock.bind(('', 0)) # '' = INADDR_ANY .puerto libre mayor que 32768 definido en /proc/sys/net/ipv4/ip_local_port_range 
            listening_port = listen_sock.getsockname()[1]
            listen_sock.listen(MAX_CONNECTION_QUEUE) #conexiones pendientes maximas 5

            #funcion para atender las peticiones de transferencia de archivos
            def servicio_archivos():
                while True:
                    try:
                        new_client_sock, addr = listen_sock.accept()
                        with new_client_sock:
                            # recibir operacion
                            op = b''
                            while True:
                                c = new_client_sock.recv(1)
                                if c == b'\0' or not c:
                                    break
                                op += c
                            if op.decode() != "GET_FILE":
                                new_client_sock.sendall(b'2')  # operacion no valida
                                continue

                            # recibir ruta del archivo
                            file_path = b''
                            while True:
                                c = new_client_sock.recv(1)
                                if c == b'\0' or not c:
                                    break
                                file_path += c
                            file_path = file_path.decode()

                            # solo se sirven ficheros publicados; cualquier otra ruta se trata
                            # como inexistente para no revelar que existe en el disco
                            if file_path not in client._published_files:
                                new_client_sock.sendall(b'1')
                                continue

                            try:
                                with open(file_path, "rb") as f:
                                    content = f.read()
                                    # enviar codigo de exito
                                    new_client_sock.sendall(b'0')
                                    # enviar tamano del archivo como cadena terminada en '\0'
                                    size_str = f"{len(content)}\0".encode()
                                    new_client_sock.sendall(size_str)
                                    # enviar contenido
                                    new_client_sock.sendall(content)
                            except FileNotFoundError:
                                new_client_sock.sendall(b'1')  # archivo no encontrado
                            except Exception:
                                new_client_sock.sendall(b'2')  # otro error
                    except Exception:
                        break  # salir del bucle si hay un error general (comom cierre del servidor)

            #crear hilo de servicio
            hilo_servicio = threading.Thread(target=servicio_archivos, daemon=True)
            #daemon True hace que si el hilo principal termina, el resto de hilos tambien
            hilo_servicio.start()

            #conectar al servidor
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
                sock.connect((client._server, client._port))

                #enviar en orden "CONNECT", user_name y port
                sock.sendall(b"CONNECT\0")

                # obtener la fecha y hora del webservice y enviarla
                client.send_fecha_hora(sock)

                sock.sendall(user.encode('utf-8') + b"\0")
                sock.sendall(str(listening_port).encode('utf-8') + b"\0")

                #esperar respuesta del servidor
                respuesta = sock.recv(1)
                #decodificar respuesta
                respuesta = respuesta.decode()

                if (respuesta == '0'):
                    client._user_name_after_connect = user #guardar nombre de usuario con el que se ha conectado
                    client.is_connected = True #cambia a conectado
                    client._listen_sock = listen_sock #se almacena el socket de servicio
                    client._hilo_servicio = hilo_servicio #se almacena el hilo de servicio
                    # recuperar del servidor los ficheros que el usuario ya tenia publicados
                    # (de sesiones anteriores), para poder seguir sirviendolos
                    _, own_files = client._request_content(user, user)
                    client._published_files = set(own_files or [])
                    print("CONNECT OK")
                    return client.RC.OK
                elif (respuesta == '1'):
                    print("CONNECT FAIL, USER DOES NOT EXIST")
                    return client.RC.USER_ERROR
                elif (respuesta == '2'):
                    print("USER ALREADY CONNECTED")
                    return client.RC.USER_ERROR
                else:
                    print("CONNECT FAIL")
                    return client.RC.ERROR        

        except Exception:
            print(f"CONNECT FAIL")
            return client.RC.ERROR

    @staticmethod

    def  disconnect(user) :
        # primero parar el hilo y cerrar el socket de escucha
        try:
            if client._listen_sock:
                client._listen_sock.close()
                client._listen_sock = None
            if client._hilo_servicio:
                client._hilo_servicio.join(timeout=1)
                client._hilo_servicio = None
        except Exception:
            pass  # ignorar errores al cerrar

        # ahora contactar con el servidor
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
                sock.connect((client._server, client._port))
                sock.sendall(b"DISCONNECT\0")

                # obtener la fecha y hora del webservice y enviarla
                client.send_fecha_hora(sock)

                sock.sendall(user.encode('utf-8') + b"\0")

                respuesta = sock.recv(1).decode()

                if respuesta == '0':
                    print("DISCONNECT OK")
                    client._user_name_after_connect = None
                    client.is_connected = False
                    return client.RC.OK
                elif respuesta == '1':
                    print("DISCONNECT FAIL, USER DOES NOT EXIST")
                    return client.RC.USER_ERROR
                elif respuesta == '2':
                    print("DISCONNECT FAIL, USER NOT CONNECTED")
                    return client.RC.USER_ERROR
                else:
                    print("DISCONNECT FAIL")
                    return client.RC.ERROR

        except Exception:
            print("DISCONNECT FAIL")
            return client.RC.ERROR



    @staticmethod

    def  publish(fileName,  description) :
        if((len(fileName) > MAX_FILE_NAME_LENGTH-1) or (len(description) > MAX_FILE_NAME_LENGTH-1)):
            print(f"PUBLISH FAIL")
            return client.RC.USER_ERROR
        
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
                #conectar al server
                sock.connect((client._server,client._port))

                #envio de operacion
                sock.sendall(b"PUBLISH\0")

                # obtener la fecha y hora del webservice y enviarla
                client.send_fecha_hora(sock)

                #envio de user_name con el que se ha conectado
                sock.sendall(client._user_name_after_connect.encode('utf-8') + b"\0")

                #envio file_name
                sock.sendall(fileName.encode('utf-8') + b"\0")

                #envio description
                sock.sendall(description.encode('utf-8') + b"\0")

                #recibir respuesta del server
                respuesta = sock.recv(1)
                #decodificar respuesta
                respuesta = respuesta.decode()

                if (respuesta == '0'):
                    #publish correcto
                    client._published_files.add(fileName)
                    print(f"PUBLISH OK")
                    return client.RC.OK
                elif (respuesta == '1'):
                    #no existe usuario
                    print(f"PUBLISH FAIL, USER DOES NOT EXIST")
                    return client.RC.USER_ERROR
                elif (respuesta == '2'):
                    print(f"PUBLISH FAIL, USER NOT CONNECTED")
                    return client.RC.USER_ERROR
                elif (respuesta == '3'):
                    print(f"PUBLISH FAIL, CONTENT ALREADY PUBLISHED")
                    return client.RC.USER_ERROR
                else:
                    #cualquier otro error
                    print(f"PUBLISH FAIL")
                    return client.RC.ERROR
                
        except Exception:
            print(f"PUBLISH FAIL")
            return client.RC.ERROR



    @staticmethod

    def  delete(fileName) :

        if((len(fileName) > MAX_FILE_NAME_LENGTH-1)):
            print(f"DELETE FAIL")
            return client.RC.USER_ERROR
        
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
                #conectar al server
                sock.connect((client._server,client._port))

                #envio de operacion
                sock.sendall(b"DELETE\0")

                # obtener la fecha y hora del webservice y enviarla
                client.send_fecha_hora(sock)

                #envio de user_name con el que se ha conectado
                sock.sendall(client._user_name_after_connect.encode('utf-8') + b"\0")

                #envio file_name
                sock.sendall(fileName.encode('utf-8') + b"\0")

                #recibir respuesta del server
                respuesta = sock.recv(1)
                #decodificar respuesta
                respuesta = respuesta.decode()

                if (respuesta == '0'):
                    #delete correcto
                    client._published_files.discard(fileName)
                    print(f"DELETE OK")
                    return client.RC.OK
                elif (respuesta == '1'):
                    #no existe usuario
                    print(f"DELETE FAIL, USER DOES NOT EXIST")
                    return client.RC.USER_ERROR
                elif (respuesta == '2'):
                    print(f"DELETE FAIL, USER NOT CONNECTED")
                    return client.RC.USER_ERROR
                elif (respuesta == '3'):
                    print(f"DELETE FAIL, CONTENT NOT PUBLISHED")
                    return client.RC.USER_ERROR
                else:
                    #cualquier otro error
                    print(f"DELETE FAIL")
                    return client.RC.ERROR
                
        except Exception:
            print(f"DELETE FAIL")
            return client.RC.ERROR




    @staticmethod

    def  listusers() :
        try:
            #crear socket tcp
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
                #conectar con el servidor
                sock.connect((client._server, client._port))

                #enviar operacion "LIST_USER" + \0
                sock.sendall(b"LIST_USERS\0")

                # obtener la fecha y hora del webservice y enviarla
                client.send_fecha_hora(sock)

                #enviar el user_name
                sock.sendall(client._user_name_after_connect.encode('utf-8') + b"\0")

                #recibir respuesta del server
                respuesta = sock.recv(1)

                #decodificar bytes de la respuesta
                respuesta = respuesta.decode()

                if respuesta == '0':
                    print(f"LIST_USERS OK")
                    #recibir numero de usuarios
                    num_users = b''
                    while True:
                        c = sock.recv(1)
                        if c == b'\0':
                            break
                        num_users += c
                    try:
                        total = int(num_users.decode())
                    except:
                        print("LIST_USERS FAIL")
                        return client.RC.ERROR

                    #recibir datos de cada usuario
                    for _ in range(total):
                        #recibir username
                        uname = b''
                        while True:
                            c = sock.recv(1)
                            if c == b'\0':
                                break
                            uname += c
                        uname = uname.decode()

                        #recibir IP
                        ip = b''
                        while True:
                            c = sock.recv(1)
                            if c == b'\0':
                                break
                            ip += c
                        ip = ip.decode()

                        #recibir puerto
                        port = b''
                        while True:
                            c = sock.recv(1)
                            if c == b'\0':
                                break
                            port += c
                        port = port.decode()

                        print(f"    {uname} {ip} {port}")

                    return client.RC.OK
                elif respuesta == '1':
                    print(f"LIST_USERS FAIL, USER DOES NOT EXIST")
                    return client.RC.USER_ERROR
                elif respuesta == '2':
                    print(f"LIST_USERS FAIL, USER IS NOT CONNECTED")
                    return client.RC.USER_ERROR
                else:
                    print(f"LIST_USERS FAIL")
                    return client.RC.ERROR

        except Exception:
            print(f"LIST_USERS FAIL")
            return client.RC.ERROR



    @staticmethod

    def _recv_str(sock):
        """Lee del socket una cadena terminada en el byte nulo. Lanza ConnectionError si se cierra antes."""
        data = b''
        while True:
            c = sock.recv(1)
            if not c:
                raise ConnectionError("conexion cerrada antes de terminar el mensaje")
            if c == b'\0':
                return data.decode()
            data += c

    @staticmethod

    def _request_content(caller, target):
        """
        Pide al servidor la lista de ficheros publicados por 'target' en nombre de 'caller'.
        Devuelve (codigo_respuesta, lista_de_ficheros); la lista es None si el codigo no es '0'.
        Si hay un error de comunicacion devuelve (None, None).
        """
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
                sock.connect((client._server, client._port))
                sock.sendall(b"LIST_CONTENT\0")
                # obtener la fecha y hora del webservice y enviarla
                client.send_fecha_hora(sock)
                sock.sendall(caller.encode('utf-8') + b"\0")
                sock.sendall(target.encode('utf-8') + b"\0")

                respuesta = sock.recv(1).decode()
                if respuesta != '0':
                    return respuesta, None

                total_files = int(client._recv_str(sock))
                return '0', [client._recv_str(sock) for _ in range(total_files)]
        except Exception:
            return None, None

    @staticmethod

    def  listcontent(user) :
        if((len(user) > MAX_USER_NAME_LENGTH-1)):
            print(f"LIST_CONTENT FAIL")
            return client.RC.ERROR

        if client._user_name_after_connect is None:
            print(f"LIST_CONTENT FAIL, USER NOT CONNECTED")
            return client.RC.USER_ERROR

        respuesta, files = client._request_content(client._user_name_after_connect, user)

        if (respuesta == '0'):
            #LIST_CONTENT correcto
            print(f"LIST_CONTENT OK")
            for fname in files:
                print(f"    {fname}")
            return client.RC.OK
        elif (respuesta == '1'):
            #no existe usuario
            print(f"LIST_CONTENT FAIL, USER DOES NOT EXIST")
            return client.RC.USER_ERROR
        elif (respuesta == '2'):
            print(f"LIST_CONTENT FAIL, USER NOT CONNECTED")
            return client.RC.USER_ERROR
        elif (respuesta == '3'):
            print(f"LIST_CONTENT FAIL, REMOTE USER DOES NOT EXIST")
            return client.RC.USER_ERROR
        else:
            #cualquier otro error
            print(f"LIST_CONTENT FAIL")
            return client.RC.ERROR



    @staticmethod
    def getfile(user, remote_FileName, local_FileName):
        try:
            # Buscar IP y puerto del usuario remoto en la lista
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock_server:
                #esta parte repite el proceso de list_users para extraer la ip y puertos deseados
                sock_server.connect((client._server, client._port))
                sock_server.sendall(b"LIST_USERS\0")

                # obtener la fecha y hora del webservice y enviarla
                client.send_fecha_hora(sock_server)

                sock_server.sendall(client._user_name_after_connect.encode('utf-8') + b"\0")
                respuesta = sock_server.recv(1).decode()
                if respuesta != '0':
                    print("GET_FILE FAIL")
                    return client.RC.ERROR
                # leer numero de usuarios
                n_users = b""
                while True:
                    c = sock_server.recv(1)
                    if c == b"\0":
                        break
                    n_users += c

                try:
                    n_users = int(n_users.decode())
                except:
                    print("GET_FILE FAIL")
                    return client.RC.ERROR
                
                ip = None
                port = None
                for _ in range(n_users):
                    #por cada user
                    name, addr, p = b"", b"", b""
                    while True:
                        #primero name
                        c = sock_server.recv(1)
                        if c == b"\0":
                            break
                        name += c

                    while True:
                        #despues ip
                        c = sock_server.recv(1)
                        if c == b"\0":
                            break
                        addr += c

                    while True:
                        #despues puerto
                        c = sock_server.recv(1)
                        if c == b"\0":
                            break
                        p += c

                    if name.decode() == user:
                        ip = addr.decode()
                        port = int(p.decode())
                        break
            if ip is None or port is None:
                print("GET_FILE FAIL")
                return client.RC.ERROR
        except:
            print("GET_FILE FAIL")
            return client.RC.ERROR

        # conectar al cliente remoto
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
                sock.connect((ip, port))

                # enviar "GET FILE"
                sock.sendall(b"GET_FILE\0")

                # enviar ruta remota
                sock.sendall(remote_FileName.encode("utf-8") + b"\0")

                # recibir codigo de resultado
                result_code = sock.recv(1).decode()

                if result_code == '1':
                    print("GET_FILE FAIL, FILE NOT EXIST")
                    return client.RC.USER_ERROR
                elif result_code != '0':
                    print("GET_FILE FAIL")
                    return client.RC.ERROR

                # recibir tamano del archivo
                file_size_str = b""
                while True:
                    c = sock.recv(1)
                    if c == b"\0":
                        break
                    file_size_str += c
                try:
                    file_size = int(file_size_str.decode())
                except:
                    print("GET_FILE FAIL")
                    return client.RC.ERROR

                # recibir contenido y escribir a local_FileName
                with open(local_FileName, "wb") as f:
                    bytes_received = 0
                    while bytes_received < file_size:
                        chunk = sock.recv(min(4096, file_size - bytes_received)) #min para no pedir de mas cuando se llega al final
                        if not chunk:
                            raise Exception("GET FILE FAIL")
                        f.write(chunk)
                        bytes_received += len(chunk)

                print("GET_FILE OK")
                return client.RC.OK

        except:
            # Si ocurre error, borrar el fichero
            try:
                import os
                if os.path.exists(local_FileName):
                    os.remove(local_FileName)
            except:
                pass
            print("GET_FILE FAIL")
            return client.RC.ERROR

    # *

    # **

    # * @brief Command interpreter for the client. It calls the protocol functions.

    @staticmethod

    def shell():



        while (True) :

            try :

                command = input("c> ")

                line = command.split(" ")

                if (len(line) > 0):



                    line[0] = line[0].upper()



                    if (line[0]=="REGISTER") :

                        if (len(line) == 2) :

                            client.register(line[1])

                        else :

                            print("Syntax error. Usage: REGISTER <userName>")



                    elif(line[0]=="UNREGISTER") :

                        if (len(line) == 2) :

                            client.unregister(line[1])

                        else :

                            print("Syntax error. Usage: UNREGISTER <userName>")



                    elif(line[0]=="CONNECT") :

                        if (len(line) == 2) :

                            client.connect(line[1])

                        else :

                            print("Syntax error. Usage: CONNECT <userName>")

                    

                    elif(line[0]=="PUBLISH") :

                        if (len(line) >= 3) :

                            #  Remove first two words

                            description = ' '.join(line[2:])

                            client.publish(line[1], description)

                        else :

                            print("Syntax error. Usage: PUBLISH <fileName> <description>")



                    elif(line[0]=="DELETE") :

                        if (len(line) == 2) :

                            client.delete(line[1])

                        else :

                            print("Syntax error. Usage: DELETE <fileName>")



                    elif(line[0]=="LIST_USERS") :

                        if (len(line) == 1) :

                            client.listusers()

                        else :

                            print("Syntax error. Use: LIST_USERS")



                    elif(line[0]=="LIST_CONTENT") :

                        if (len(line) == 2) :

                            client.listcontent(line[1])

                        else :

                            print("Syntax error. Usage: LIST_CONTENT <userName>")



                    elif(line[0]=="DISCONNECT") :

                        if (len(line) == 2) :

                            client.disconnect(line[1])

                        else :

                            print("Syntax error. Usage: DISCONNECT <userName>")



                    elif(line[0]=="GET_FILE") :

                        if (len(line) == 4) :

                            client.getfile(line[1], line[2], line[3])

                        else :

                            print("Syntax error. Usage: GET_FILE <userName> <remote_fileName> <local_fileName>")



                    elif(line[0]=="QUIT") :

                        if (len(line) == 1) :
                            if client.is_connected and client._user_name_after_connect:
                                client.disconnect(client._user_name_after_connect) #hacer desconexion si estaba conectado
                            break

                        else :

                            print("Syntax error. Use: QUIT")

                    else :

                        print("Error: command " + line[0] + " not valid.")

            except Exception as e:

                print("Exception: " + str(e))



    # *

    # * @brief Prints program usage

    @staticmethod

    def usage() :

        print("Usage: python3 client.py -s <server> -p <port>")





    # *

    # * @brief Parses program execution arguments

    @staticmethod

    def  parseArguments(argv) :

        parser = argparse.ArgumentParser()

        parser.add_argument('-s', type=str, required=True, help='Server IP')

        parser.add_argument('-p', type=int, required=True, help='Server Port')

        args = parser.parse_args()



        if (args.s is None):

            parser.error("Usage: python3 client.py -s <server> -p <port>")

            return False



        if ((args.p < 1024) or (args.p > 65535)):

            parser.error("Error: Port must be in the range 1024 <= port <= 65535");

            return False;

        

        client._server = args.s

        client._port = args.p



        return True





    # ******************** MAIN *********************

    @staticmethod

    def main(argv) :

        if (not client.parseArguments(argv)) :

            client.usage()

            return



        #  Write code here
        client._ws_client = Client("http://127.0.0.1:8000/?wsdl") #para poder conectar con el webservice
        client.shell()

        print("+++ FINISHED +++")

    



if __name__=="__main__":

    client.main([])