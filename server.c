#include "logger.h" //servidor rpc para loggear
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>   // para strtol
#include <errno.h>    // para errno
#include <signal.h>
#include <stdbool.h> // para sigint
#include <pthread.h> //mutex
#include <string.h> //strncmp
#include <stdlib.h> //malloc y free
#include <unistd.h> //para write, close y read

#define MAX_OPERATION_LENGTH 16
#define MAX_DATETIME_LENGTH 24
#define h_addr h_addr_list[0] /* para compatibilidad con versiones anteriores (ademas evitar error de vscode) */
#define MAX_USER_NAME_LENGTH	256
#define MAX_FILE_NAME_LENGTH	256
#define MAX_FILE_DESC_LENGTH	256

//============================================STRUCTS====================================================

typedef struct FileNode{ 
	char 	file_name[MAX_FILE_NAME_LENGTH];
	char	file_desc[MAX_FILE_DESC_LENGTH];
	struct 	FileNode *next; 
} FileNode;

typedef FileNode * FileList;

typedef struct ClientNode{ 
	char 	user_name[MAX_USER_NAME_LENGTH];
	uint32_t IP;
	uint16_t port;
	int status; // 0 desconectado. 1 conectado.
	FileList files;
	pthread_mutex_t files_mutex; //mutex para proteger su propia lista de archivos
	struct 	ClientNode *next;
} ClientNode;

typedef struct {
	ClientNode * head;
	pthread_mutex_t mutex;
} ClientsList;

//========================================VARIABLES GLOBALES====================================================

ClientsList *lista_global_clientes;  // variable global accesible desde todas las funciones
int serv_sock = -1;  // visible globalmente para main() y handle_sigint()

//========================================BACKEND FUNCTIONS=====================================================

/**
 * @brief Esta llamada borra el FileList de un nodo ClientNode, ademas NO
 * TIENE CONTROL DE CONCURRENCIA, por tanto se asume que hay que hacer un
 * mutex_lock(client->file_mutex) antes de invocarla.
 * 
 * 
 * @param void
 * @return void
 */
void file_list_destroy(FileList list){
    FileNode *aux = list;
    
    while(aux != NULL){
        FileNode *tmp = aux;
        aux = aux->next;
        free(tmp);
    }
}

/**
 * @brief Esta llamada permite inicializar la lista de clientes
 * 
 * @return puntero a ClientList
 * @retval puntero cualquiera en caso de exito.
 * @retval NULL en caso de error.
 */
ClientsList * clients_list_init(){
    ClientsList * list = malloc(sizeof(ClientsList));
    if (!list) {
        perror("Error al inicializar la lista de clientes");
        return NULL;
    }
    list->head = NULL;
    pthread_mutex_init(&list->mutex, NULL);
    return list;
}

/**
 * @brief Esta llamada comprueba si existe un ClientNode con username dado por param user_name
 * en caso de existir, devuelve 1, en caso contrario 0. Es static porque no hay llamadas al
 * servidor que requieran de esta llamada por tanto debe estar restringida al dominio de su
 * implementacion. NO TIENE CONTROL DE CONCURRENCIA INTERNO: quien la llama debe tener
 * bloqueado list->mutex, para que la comprobacion y la operacion posterior sean atomicas.
 *
 *
 * @param user_name Es el nombre de usuario que se comprueba si esta en la lista
 * @return int
 * @retval 0 si NO se encuentra en ClientsList
 * @retval 1 si se encuentra en ClientsList
 */
static int clients_list_exist(ClientsList *list, const char* user_name){
    ClientNode * curr = list->head;
    while (curr){
        if (strncmp(curr->user_name, user_name, MAX_USER_NAME_LENGTH) == 0)
            return 1;
        curr = curr->next;
    }
    return 0;
}

/**
 * @brief llamada comprueba si existe en la lista algun fichero con el nombre file_list
 * NO TIENE CONTROL DE CONCURRENCIA INTERNO
 * 
 * @param list lista de ficheros
 * @param file_name nombre de fichero que se busca
 * @return int
 * @retval 0 si no existe
 * @retval 1 si existe
 */
static int file_list_exist(FileList list, const char* file_name){
    FileNode * curr = list;
    while (curr){
        if (strncmp(curr->file_name, file_name, MAX_FILE_NAME_LENGTH) == 0)
            return 1;
        curr = curr->next;
    }
    return 0;

}

/**
 * @brief llamada anade fichero, a la lista de ficheros, con nombre file_name 
 * y file_desc de dicho fichero
 * NO TIENE CONTROL DE CONCURRENCIA INTERNO PARA LLAMARLA HAY ANTES QUE HACER
 * lock del mutexfiles y despues de llamarla unlock de mutexfiles
 * 
 * @param list lista de ficheros
 * @param file_name nombre de fichero que se anade
 * @param file_desc descripcion de fichero que se anade
 * @return int
 * @retval 0 exito al anadir
 * @retval -1 error al anadir
 */
static int file_list_add_file(FileList * list, const char* file_name, const char* file_desc){
    if(!list || !file_name || !file_desc) 
        return -1; //list o user_name NULL
    
    FileNode * new_file;
    new_file = (FileNode *) malloc(sizeof(FileNode));
    if (!new_file)
        return -1; //error al abrir memoria para nuevo fichero
        
    snprintf(new_file->file_name, MAX_FILE_NAME_LENGTH, "%s", file_name);
    snprintf(new_file->file_desc, MAX_FILE_DESC_LENGTH, "%s", file_desc);

    new_file->next = *list;
    *list = new_file;

    return 0;
}

/**
 * @brief llamada borra fichero, de la lista de ficheros, con nombre file_name 
 * NO TIENE CONTROL DE CONCURRENCIA INTERNO PARA LLAMARLA HAY ANTES QUE HACER
 * lock del mutexfiles y despues de llamarla unlock de mutexfiles
 * 
 * @param list lista de ficheros
 * @param file_name nombre de fichero que se anade
 * @return int
 * @retval 0 exito al anadir
 * @retval -1 error al anadir
 * @retval -3 no existe fichero
 */
static int file_list_delete_file(FileList * list, const char* file_name){
    if (!list || !file_name)
        return -1;

    FileNode * back_file = NULL;
    FileNode * curr_file = *list;

    while (curr_file){
        if (strncmp(curr_file->file_name, file_name, MAX_FILE_NAME_LENGTH) == 0){            
            if (back_file != NULL){
                //caso no es primer nodo
                back_file->next = curr_file->next;
            } else {
                //caso en el que el primer nodo es el encontrado
                *list = curr_file->next;
            }

            free(curr_file);
            return 0;
        }
        back_file = curr_file;
        curr_file = curr_file->next;
    }

    return -3; //fichero no se ha encontrado
}

/**
 * @brief Esta llamada anade un nuevo usuario con user_name en ClientsList, si lo anade
 * exitosamente return 0, si hay error return -1 y si el usuario ya se encuentra en la lista
 * return -2
 * 
 * 
 * @param user_name Es el nombre de usuario que se anade a ClientList
 * @return int 
 * @retval 0 si anadido exitosamente
 * @retval -1 si error al anadir usuario
 * @retval -2 si el usuario ya se encuentra en la lista
 */
int clients_list_register(ClientsList *list, const char* user_name){
    if(!list || !user_name)
        return -1; //list o user_name NULL

    // la comprobacion de existencia y la insercion se hacen en la misma seccion critica:
    // si se separan, dos registros simultaneos del mismo nombre podrian tener exito ambos
    pthread_mutex_lock(&list->mutex);
    if (clients_list_exist(list, user_name)){
        pthread_mutex_unlock(&list->mutex);
        return -2; //ya existe el usuario en la lista
    }

    ClientNode * new_client;
    new_client = (ClientNode *) malloc(sizeof(ClientNode));
    if (!new_client){
        pthread_mutex_unlock(&list->mutex);
        return -1; //error al abrir memoria para nuevo cliente
    }

    snprintf(new_client->user_name, MAX_USER_NAME_LENGTH, "%s", user_name);
    new_client->IP = 0;
    new_client->port = 0;
    new_client->status = 0;
    new_client->files = NULL;
    pthread_mutex_init(&new_client->files_mutex, NULL);

    new_client->next = list->head;
    list->head = new_client;
    pthread_mutex_unlock(&list->mutex);

    return 0;
}

/**
 * @brief Esta llamada borra a un usuario, recorriendo la lista, se usa strncmp
 * para comparar param user_name con el de cada nodo, en caso de no encontrarse
 * sale del bucle y devuelve -2 lo que significa que el usuario dado por user_name
 * no se encuentra en la lista. Ademas se usa file_list_destroy para borrar los ficheros
 * del usuario encontrado.
 * 
 * 
 * @param user_name Es el nombre de usuario que se borrar de ClientList
 * @return int 
 * @retval 0 si borrado exitosamente
 * @retval -1 si error al borrar usuario
 * @retval -2 si el usuario no se encuentra en ClientsList
 */
int clients_list_unregister(ClientsList *list, const char* user_name){
    if (!list || !user_name)
        return -1;

    pthread_mutex_lock(&list->mutex);
    ClientNode * back_usr = NULL;
    ClientNode * curr_usr = list->head;

    while (curr_usr){
        if (strncmp(curr_usr->user_name, user_name, MAX_USER_NAME_LENGTH) == 0){
            pthread_mutex_lock(&curr_usr->files_mutex);
            
            if (back_usr != NULL){
                //caso no es primer nodo
                back_usr->next = curr_usr->next;
            } else {
                //caso en el que el primer nodo es el encontrado
                list->head = curr_usr->next;
            }

            if (curr_usr->files != NULL){ //el usuario tiene ficheros que tambien hay que borrar
                file_list_destroy(curr_usr->files);
            }
            pthread_mutex_unlock(&curr_usr->files_mutex);
            pthread_mutex_destroy(&curr_usr->files_mutex); //liberar memoria de mutex de files del usr
            free(curr_usr);

            pthread_mutex_unlock(&list->mutex);
            return 0;
        }
        back_usr = curr_usr;
        curr_usr = curr_usr->next;
    }

    pthread_mutex_unlock(&list->mutex);
    return -2; //usuario no se ha encontrado
}

/**
 * @brief Este servicio borra espacio de memoria ClientsList teniendo
 * en cuenta si cada cliente tiene tambien ficheros por lo que por cada
 * cliente llama a file_list_destroy
 * 
 * 
 * @param list ClientsList a borrar
 */
void clients_list_destroy(ClientsList *list){
    if(list == NULL) //si ya esta vacia salir para evitar acceder a mutex que no existe
        return;
    
    pthread_mutex_lock(&list->mutex);
    ClientNode *aux = list->head;

    while(aux != NULL){
        ClientNode * temp = aux; //declaracion dentro del bucle porque solo es util para el bucle y compilador la borra cuando termina el bucle
        aux = aux->next;
        pthread_mutex_lock(&temp->files_mutex);
        //no hay que comprobar si aux->files != NULL porque ya lo hace file_list_destroy
        file_list_destroy(temp->files);
        pthread_mutex_unlock(&temp->files_mutex);
        pthread_mutex_destroy(&temp->files_mutex);

        free(temp);
    }
    pthread_mutex_unlock(&list->mutex);
    pthread_mutex_destroy(&list->mutex);

    free(list);
}

/**
 * @brief Esta llamada hace la conexion de un cliente en ClientsList, hace
 * mutex_lock sobre el mutex de list, y busca el ClientNode con user_name como
 * parametro de entrada, para dicho usuario hace que su IP, sea param, su port
 * es param y cambia su status a 1
 * 
 * @param list es la ClientsList sobre la que se ejecuta la llamada
 * @param user_name Es el usuario que se busca en ClientsList
 * @param IP es el valor al que se iguala la IP del cliente encontrado
 * @param port es el valor al que se iguala el port del cliente encontrado
 * 
 * @return int
 * @retval 0 usuario conectado existosamente
 * @retval -1 cualquier tipo de error al hacer la llamada
 * @retval -2 usuario no existe en ClientsList
 * @retval -3 usuario ya conectado
 */
int clients_list_connect(ClientsList* list, char* user_name, uint32_t IP, uint16_t port){
    //errores con el rango de los valores
    if (!list || !user_name || IP == 0 || port == 0)
        return -1;
    
    pthread_mutex_lock(&list->mutex);
    ClientNode * curr_usr = list->head;

    while(curr_usr){
        if(strncmp(curr_usr->user_name, user_name, MAX_USER_NAME_LENGTH) == 0){
            if (curr_usr->status == 1){
                pthread_mutex_unlock(&list->mutex);
                return -3; //usuario ya esta conectado
            }
            //a partir de aqui ya se sabe que no esta conectado asi que no hay que comprobarlo
            curr_usr->IP = IP;
            curr_usr->port = port;
            curr_usr->status = 1;

            pthread_mutex_unlock(&list->mutex);
            return 0; //se ha encontrado y se ha conectado
        }
        curr_usr = curr_usr->next;
    }

    pthread_mutex_unlock(&list->mutex);
    return -2; //usuario no encontrado
}

/**
 * @brief funcion anade un fichero y su descripcion a la lista de ficheros de un usuario
 * IMPORTANTE como busca sobre ClientsList tiene dicho prefijo, hay otra funcion mismo nombre
 * de metodo pero que busca sobre FileList
 * 
 * @param list lista de usuarios
 * @param user_name usuario al que se le anade fichero
 * @param file_name nombre del fichero que se anade
 * @param file_desc descripcion del fichero que se anade
 * 
 * @return int
 * @retval 0 fichero anadido exitosamente
 * @retval -1 error al anadir fichero
 * @retval -2 usuario no encontrado
 * @retval -3 fichero ya esta anadido a dicho usuario
 * @retval -4 usuario no esta conectado
 */
int clients_list_add_file(ClientsList* list, char* user_name, char* file_name, char* file_desc){
    //comprobacion basica
    if(!list || !user_name || !file_name || !file_desc)
        return -1;

    pthread_mutex_lock(&list->mutex);
    ClientNode * curr_usr = list->head;

    while(curr_usr){
        if(strncmp(curr_usr->user_name, user_name, MAX_USER_NAME_LENGTH) == 0){
            if (curr_usr->status == 0){
                pthread_mutex_unlock(&list->mutex);
                return -4; //usuario no esta conectado
            }
            //a partir de aqui ya se sabe que si esta conectado asi que no hay que comprobarlo

            pthread_mutex_lock(&curr_usr->files_mutex);
            //primero comprobar que no existe el fichero
            if (file_list_exist(curr_usr->files, file_name)){
                pthread_mutex_unlock(&curr_usr->files_mutex);
                pthread_mutex_unlock(&list->mutex);
                return -3; //fichero ya esta publicado
            }

            //anadir fichero
            int ret = file_list_add_file(&curr_usr->files, file_name, file_desc);
            pthread_mutex_unlock(&curr_usr->files_mutex);
            pthread_mutex_unlock(&list->mutex);
            if (ret != 0){
                return -1; //error al anadir fichero
            }
            return 0; //se ha encontrado y se ha conectado
        }
        curr_usr = curr_usr->next;
    }

    pthread_mutex_unlock(&list->mutex);
    return -2; //usuario no encontrado
}

/**
 * @brief anade borra un fichero con nombre file_name de la lista de ficheros del user_name
 * 
 * @param list lista de clientes
 * @param user_name usuario que se busca en la lista de usuarios
 * @param file_name nombre del fichero que se borra
 * 
 * @return int
 * @retval 0 exito al borrar
 * @retval -1 cualquier error
 * @retval -2 el usuario no existe
 * @retval -3 el fichero no existe en la lista de ficheros del usuario
 * @retval -4 el usuario no esta conectado
 */
int clients_list_delete_file(ClientsList* list, char* user_name, char* file_name){
    //errores con el rango de los valores
    if (!list || !user_name || !file_name)
        return -1;
    
    pthread_mutex_lock(&list->mutex);
    ClientNode * curr_usr = list->head;

    while(curr_usr){
        if(strncmp(curr_usr->user_name, user_name, MAX_USER_NAME_LENGTH) == 0){
            if (curr_usr->status == 0){
                pthread_mutex_unlock(&list->mutex);
                return -4; //usuario no esta conectado
            }
            //a partir de aqui ya se sabe que esta conectado asi que no hay que comprobarlo
            pthread_mutex_lock(&curr_usr->files_mutex);
            int result = file_list_delete_file(&curr_usr->files, file_name);
            pthread_mutex_unlock(&curr_usr->files_mutex);
            pthread_mutex_unlock(&list->mutex);
            return result; 
            //result = 0 se ha encontrado y se ha borrado el file
            //result = -3 fichero no se ha encontrado
            //result = -1 error
        }
        curr_usr = curr_usr->next;
    }

    pthread_mutex_unlock(&list->mutex);
    return -2; //usuario no encontrado
}

/**
 * @brief cuenta el numero de usuarios en la lista y ademas, comprueba que el
 * usuario 'user_name' se encuentra en la lista y esta conectado
 * NO LLEVA CONTROL DE CONCURRENCIA	se deja fuera para que no haya cambios en la lista
 * entre que se cuenta y se envia el resultado al usuario
 * 
 * @ret int
 * @retval >=0 quiere decir que el usuario se encuentra, en la lista, esta conectado y
 * 			se devuelve el numero de usuarios conectados en la lista
 * @retval -1 cualquier error
 * @retval -2 usuario user_name no esta conectado
 * @retval -3 usuario user_name no se encuentra en la lista
 */
int clients_list_count_users(ClientsList* list, char* user_name){
    //comprobacion basica de valores
    if (!list || !user_name)
        return -1;

    int ret_val = 0;

    ClientNode *curr = list->head;
    ClientNode *requester = NULL;

    while (curr) {
        if (strncmp(curr->user_name, user_name, MAX_USER_NAME_LENGTH) == 0) //se encuenta en la lista
            requester = curr;
        if (curr->status == 1)
            ret_val++;
        curr = curr->next;
    }

    if (!requester) {
        //si no se ha encontrado en la lista
        ret_val = -3;
    } else if (requester->status == 0) {
        //si se ha encontrado pero no esta conectado
        ret_val = -2;
    }

    return ret_val;
}

/**
 * @brief funcion busca en list, el usuario con nombre target_user_name, y copia los nombres
 * de sus ficheros en un buffer nuevo. Ademas se usa user_name como el usuario que esta haciendo
 * la llamada para comprobar si existe y si esta conectado. Los nombres se copian mientras se
 * mantienen bloqueados los mutex, de modo que el envio posterior no accede a la lista: si se
 * guardara un puntero al usuario, un UNREGISTER simultaneo podria liberarlo mientras se envia.
 *
 * @param list lista de usuarios
 * @param user_name nombre de usuario que hace la llamada, se usa para comprobar si se encuentra
 * 					en list y ademas para comprobar si esta conectado
 * @param target_user_name nombre de usuario del que se quiere listar su contenido
 * @param names_out puntero donde se devuelve un buffer con los nombres de los ficheros, cada uno
 *                  en un hueco de MAX_FILE_NAME_LENGTH bytes. Debe liberarse con free(). Es NULL
 *                  si no hay ficheros o si hay error.
 *
 * @return int
 * @retval >=0 devuelve el numero de ficheros que tiene target_user_name
 * @retval -1 cualquier error
 * @retval -2 user_name no conectado
 * @retval -3 user_name_target no encontrado
 * @retval -4 user_name no esta en la lista
 */
int clients_list_content(ClientsList* list, char* user_name, char* target_user_name, char** names_out) {
    if (!list || !user_name || !target_user_name || !names_out)
        return -1;
    *names_out = NULL;

    pthread_mutex_lock(&list->mutex);

    ClientNode *curr = list->head;
    ClientNode *caller_usr = NULL;
    ClientNode *target_usr = NULL;

    while (curr) {
        if (strncmp(curr->user_name, user_name, MAX_USER_NAME_LENGTH) == 0)
            caller_usr = curr;
        if (strncmp(curr->user_name, target_user_name, MAX_USER_NAME_LENGTH) == 0)
            target_usr = curr;
        curr = curr->next;
    }

    if (!caller_usr) {
        pthread_mutex_unlock(&list->mutex);
        return -4; // user_name no encontrado
    }

    if (caller_usr->status == 0) {
        pthread_mutex_unlock(&list->mutex);
        return -2; // user_name no está conectado
    }

    if (!target_usr) {
        pthread_mutex_unlock(&list->mutex);
        return -3; // target_user_name no encontrado
    }

    pthread_mutex_lock(&target_usr->files_mutex);

    // calcular número de ficheros publicados
    int count = 0;
    FileNode *f = target_usr->files;
    while (f) {
        count++;
        f = f->next;
    }

    // copiar los nombres de los ficheros mientras la lista sigue bloqueada
    if (count > 0) {
        char *names = malloc((size_t)count * MAX_FILE_NAME_LENGTH);
        if (!names) {
            pthread_mutex_unlock(&target_usr->files_mutex);
            pthread_mutex_unlock(&list->mutex);
            return -1;
        }
        int i = 0;
        for (f = target_usr->files; f; f = f->next, i++)
            snprintf(names + (size_t)i * MAX_FILE_NAME_LENGTH, MAX_FILE_NAME_LENGTH, "%s", f->file_name);
        *names_out = names;
    }

    pthread_mutex_unlock(&target_usr->files_mutex);
    pthread_mutex_unlock(&list->mutex);
    return count;
}

/**
 * @brief funcion desconecta (cambiar status de 1 a 0) el usuario user_name
 * solo lo desconecta si estaba conectado, en caso contrario devuelve -2
 * 
 * @param list lista de usuarios
 * @param user_name usuario a desconectar
 * 
 * @return int
 * @retval 0 exito, se ha desconectado y estaba conectado
 * @retval -1 cualquier error
 * @retval -2 el usuario no estaba conectado
 * @retval -3 el usuario no se encuentra en la lista
 */
int clients_list_disconnect(ClientsList* list, char* user_name){
    if (!list || !user_name)
        return -1;

    pthread_mutex_lock(&list->mutex);

    ClientNode *curr = list->head;
    ClientNode *user = NULL;

    while (curr) {
        if (strncmp(curr->user_name, user_name, MAX_USER_NAME_LENGTH) == 0){
            user = curr;
            break; //ya se ha encontrado el user, no es necesario seguir buscando
        }
        curr = curr->next;
    }
    
    if (!user){
        //no se ha encontrado
        pthread_mutex_unlock(&list->mutex);
        return -3;
    }

    if(user->status != 1){
        //user no estaba conectado
        pthread_mutex_unlock(&list->mutex);
        return -2;
    }
    //caso en el que user existe y esta conectado
    user->status = 0;
    pthread_mutex_unlock(&list->mutex);
    return 0;
}

//===================================================FUNCIONES AUXILIARES==========================================================


/**
 * @brief funcion para el procedimiento con el logger rpc, primero extrae la ip del servidor
 * rpc de la variable de entorno LOG_RPC_IP, construye el struct que le va a enviar y lo envia.AF_ALG
 * 
 * @param user usuario que ha hecho la operacion
 * @param operacion operacion que ha hecho el usuario
 * @param file_name en caso de que la operacion sea "PUBLISH" o "DELETE" este valor es el fichero del que se
 *                  hace la operacion
 * @param fecha es la fecha que envia el cliente desde el servicio web "FechaHoraService"
 * 
 * @return void
 */
void enviar_log_rpc(const char *user, const char *operacion, const char *file_name, const char *fecha) {
    if (!user || !operacion || !fecha) {
        fprintf(stderr, "Argumentos nulos en enviar_log_rpc\n");
        return;
    }

    CLIENT *clnt;
    log_entry entry;

    char *rpc_server_ip = getenv("LOG_RPC_IP");
    if (!rpc_server_ip) {
        fprintf(stderr, "Variable LOG_RPC_IP no definida\n");
        return;
    }

    clnt = clnt_create(rpc_server_ip, LOGGER_PROG, LOGGER_VERS, "tcp");
    if (!clnt) {
        clnt_pcreateerror(rpc_server_ip);
        return;
    }

    entry.user_name = strdup(user);
    entry.operation = strdup(operacion);
    entry.file_name = file_name ? strdup(file_name) : strdup("");
    entry.datetime = strdup(fecha);

    int respuesta_rpc = 0;
    enum clnt_stat result = log_operation_1(entry, &respuesta_rpc, clnt);
    if (result != RPC_SUCCESS) {
        fprintf(stderr, "Error en llamada RPC: %d\n", result);
    }

    free(entry.user_name);
    free(entry.operation);
    free(entry.file_name);
    free(entry.datetime);

    clnt_destroy(clnt);
}

/**
 * @brief funcion para convertir un char* a uint16_t util para el port
 * 
 * @param port_to_convert es el char* del que se extrae el contenido para el uint16_t
 * @param out_uint16_t es un puntero a uint16_t donde se almacena el valor tras convertirlo
 * 
 * @return int
 * @retval -1 el puerto es invalido, no contiene \0  o hay un problema en la conversion
 * @retval 0 se ha hecho correctamente la conversion
 */
int portChar_to_16bit(const char *port_to_convert, uint16_t *out_uint16) {
    char *endptr = NULL;
    long temp = strtol(port_to_convert, &endptr, 10);

    if (port_to_convert == endptr || *endptr != '\0' || temp < 0 || temp > 65535) {
        fprintf(stderr, "Puerto recibido inválido: %s\n", port_to_convert);
        return -1;
    }

    *out_uint16 = (uint16_t)temp;
    return 0;
}

/**
 * @brief funcion para manejar el recibo de la senal ctrl+c,
 * la correcta liberacion de memoria y el correcto cerrado del
 * socket del servidor
 * 
 * @return void
 * @param sig es la senal en este caso se se usa sigint(ctrl+c)
 */
void handle_sigint(int sig) {
    printf("\n\ns> SIGINT recibido. Cerrando servidor...\n");

    if (serv_sock != -1) {
        close(serv_sock);
    }

    clients_list_destroy(lista_global_clientes);

    printf("s> Recursos liberados. Saliendo...\n");
    exit(0);
}

/**
 * @brief funcion para enviar mensaje por un socket, con reintentos
 * para asegurar que el buffer se envia por completo
 * 
 * @param socket es el socket por el que se envia el mensaje
 * @param buffer es el contenido del mensaje que se envia
 * @param len es el tamano del buffer
 * 
 * @return int
 * @retval -1 en caso de error
 * @retval 0 en caso de haber enviado todo el buffer
 */
int sendMessage(int socket, char * buffer, int len)
{
	int r;
	int l = len;
		

	do {	
		r = write(socket, buffer, l);
		l = l -r;
		buffer = buffer + r;
	} while ((l>0) && (r>=0));
	
	if (r < 0)
		return (-1);   /* fail */
	else
		return(0);	/* full length has been sent */
}

/**
 * @brief funcion para leer de un socket fd, una cadena de caracteres
 * que termina en \n o \0
 * 
 * @param fd es el socket desde el que se lee
 * @param buffer donde se almacena la lectura del socket
 * @param n tamano total de lectura o en su defecto cuando llega \n o \0
 * 
 * @return ssize_t
 * @retval numero de caracteres (bytes) leidos
 */
ssize_t readLine(int fd, void *buffer, size_t n)
{
	ssize_t numRead;  /* num of bytes fetched by last read() */
	size_t totRead;	  /* total bytes read so far */
	char *buf;
	char ch;


	if (n <= 0 || buffer == NULL) { 
		errno = EINVAL;
		return -1; 
	}

	buf = buffer;
	totRead = 0;
	
	for (;;) {
        	numRead = read(fd, &ch, 1);	/* read a byte */

        	if (numRead == -1) {	
            		if (errno == EINTR)	/* interrupted -> restart read() */
                		continue;
            	else
			return -1;		/* some other error */
        	} else if (numRead == 0) {	/* EOF */
            		if (totRead == 0)	/* no byres read; return 0 */
                		return 0;
			else
                		break;
        	} else {			/* numRead must be 1 if we get here*/
            		if (ch == '\n')
                		break;
            		if (ch == '\0')
                		break;
            		if (totRead < n - 1) {		/* discard > (n-1) bytes */
				totRead++;
				*buf++ = ch; 
			}
		} 
	}
	
	*buf = '\0';
    	return totRead;
}

//====================================================MAIN==================================================

void * atender_peticion(void* arg){
    signal(SIGINT, handle_sigint); //
    int new_client_sock = *(int *)arg;
    free(arg);
    int err; //para control de errores
    
    char operacion[MAX_OPERATION_LENGTH] = {0}; //suficiente para almacenar cualquiera de las operaciones
    err = readLine(new_client_sock, (char *)operacion, sizeof(operacion));
    if (err <= 0){
        perror("Error al recibir operacion\n");
        close(new_client_sock);
        return NULL;
    }
    
    //recibir fecha hora
    char fecha_hora[MAX_DATETIME_LENGTH] = {0};
    err = readLine(new_client_sock, (char *)fecha_hora, sizeof(fecha_hora));
    if (err <= 0){
        perror("Error al recibir fecha_hora\n");
        close(new_client_sock);
        return NULL;
    }

    //llamada a las funciones
    if (strcmp(operacion, "REGISTER") == 0){
        /*RESPUESTA AL CLIENTE si result 0 : '0', result -1 : '2', result -2 : '1' */
        char user_name[MAX_USER_NAME_LENGTH] = {0};
        err = readLine(new_client_sock, (char *)user_name, sizeof(user_name));
        if (err <= 0){
            perror("Error al recibir user_name\n");
            close(new_client_sock);
            return NULL;
        }

        enviar_log_rpc(user_name, operacion, NULL, fecha_hora); //servidor rpc de logging
        int result = clients_list_register(lista_global_clientes, user_name);
        printf("OPERATION REGISTER FROM %s\n", user_name);

        // determinar y enviar codigo de resultado
        char respuesta;
        if (result == 0) {
            respuesta = '0'; //exito
        } else if (result == -2) {
            respuesta = '1'; // usuario ya existe en la lista
        } else {
            respuesta = '2'; // error
        }

        //enviar respuesta
        err = sendMessage(new_client_sock, (char *)&respuesta, sizeof(char));
        if (err < 0){
            perror("Error al enviar respuesta\n");
            close(new_client_sock);
            return NULL;
        }
    } else if(strcmp(operacion, "UNREGISTER") == 0){
        //RESPUESTA AL CLIENTE si result 0 : '0', result -2 : '1', result -1 : '2'
        //recibir user_name
        char user_name[MAX_USER_NAME_LENGTH] = {0};
        err = readLine(new_client_sock, (char *)user_name, sizeof(user_name));
        if (err <= 0){
            perror("Error al recibir user_name\n");
            close(new_client_sock);
            return NULL;
        }
        enviar_log_rpc(user_name, operacion, NULL, fecha_hora); //servidor rpc de logging

        int result = clients_list_unregister(lista_global_clientes, user_name);
        printf("OPERATION UNREGISTER FROM %s\n", user_name); //log local

        // determinar y enviar codigo de resultado
        char respuesta;
        if (result == 0) {
            respuesta = '0'; //exito
        } else if (result == -2) {
            respuesta = '1'; // usuario no existe en la lista
        } else {
            respuesta = '3'; // error
        }

        //enviar respuesta
        err = sendMessage(new_client_sock, (char *)&respuesta, sizeof(char));
        if (err < 0){
            perror("Error al enviar respuesta\n");
            close(new_client_sock);
            return NULL;
        }
    } else if(strcmp(operacion, "CONNECT") == 0){
        //RESPUESTA AL CLIENTE si result 0:'0', result -2:'1', result -3:'2', result -1:'3'
        //recibir user_name
        char user_name[MAX_USER_NAME_LENGTH] = {0};
        err = readLine(new_client_sock, (char *)user_name, sizeof(user_name));
        if (err <= 0){
            perror("Error al recibir user_name\n");
            close(new_client_sock);
            return NULL;
        }

        enviar_log_rpc(user_name, operacion, NULL, fecha_hora); //servidor rpc de logging

        char port_str[6] = {0};
        err = readLine(new_client_sock, (char *)port_str, sizeof(port_str));
        if (err <= 0){
            perror("Error al recibir port_str\n");
            close(new_client_sock);
            return NULL;
        }

        //pasar el char* port a el uint16_t usando funcion
        uint16_t port;
        if (portChar_to_16bit(port_str, &port) != 0) {
            // manejar el error en la conversion
            close(new_client_sock);
            return NULL;
        }

        //extraer ip del socket del cliente
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        if (getpeername(new_client_sock, (struct sockaddr*)&addr, &addr_len) < 0) {
            perror("Error al obtener IP del cliente con getpeername");
            close(new_client_sock);
            return NULL;
        }
        uint32_t ip = ntohl(addr.sin_addr.s_addr); // conversion a host byte order

        //hacer operacion del backend
        int result = clients_list_connect(lista_global_clientes, user_name, ip, port);

        //mostrar que se ha hecho la operacion
        printf("OPERATION CONNECT FROM %s IP %s:%u\n", user_name, inet_ntoa(addr.sin_addr), port);

        // determinar y enviar codigo de resultado
        char respuesta;
        if (result == 0) {
            respuesta = '0'; //exito
        } else if (result == -2) {
            respuesta = '1'; // usuario no existe en la lista
        } else if (result == -3) {
            respuesta = '2'; // user_name ya conectado
        } else {
            respuesta = '3'; // error
        }

        //enviar respuesta
        err = sendMessage(new_client_sock, (char *)&respuesta, sizeof(char));
        if (err < 0){
            perror("Error al enviar respuesta\n");
            close(new_client_sock);
            return NULL;
        }
    } else if(strcmp(operacion, "PUBLISH") == 0){
        //RESPUESTA AL CLIENTE si result 0:'0', result -2:'1', result -4:'2', result -3:'3', result -1:'4'
        //recibir user_name
        char user_name[MAX_USER_NAME_LENGTH] = {0};
        err = readLine(new_client_sock, (char *)user_name, sizeof(user_name));
        if (err <= 0){
            perror("Error al recibir user_name\n");
            close(new_client_sock);
            return NULL;
        }

        //recibir file_name
        char file_name[MAX_FILE_NAME_LENGTH] = {0};
        err = readLine(new_client_sock, (char *)file_name, sizeof(file_name));
        if (err <= 0){
            perror("Error al recibir file_name\n");
            close(new_client_sock);
            return NULL;
        }

        enviar_log_rpc(user_name, operacion, file_name, fecha_hora); //servidor rpc de logging

        //recibir file_desc
        char file_desc[MAX_FILE_DESC_LENGTH] = {0};
        err = readLine(new_client_sock, (char *)file_desc, sizeof(file_desc));
        if (err <= 0){
            perror("Error al recibir file_desc\n");
            close(new_client_sock);
            return NULL;
        }
        
        int result = clients_list_add_file(lista_global_clientes, user_name, file_name, file_desc);
        //mostrar que se ha hecho la operacion
        printf("OPERATION PUBLISH FROM %s\n", user_name);

        // determinar y enviar codigo de resultado
        char respuesta;
        if (result == 0) {
            respuesta = '0'; //exito
        } else if (result == -2) {
            respuesta = '1'; // usuario no existe en la lista
        } else if (result == -4) {
            respuesta = '2'; // user_name no conectado
        } else if (result == -3) {
            respuesta = '3'; //fichero ya se encuentra en la lista del usuario
        } else {
            respuesta = '4'; //error
        }

        //enviar respuesta
        err = sendMessage(new_client_sock, (char *)&respuesta, sizeof(char));
        if (err < 0){
            perror("Error al enviar respuesta\n");
            close(new_client_sock);
            return NULL;
        }
    } else if(strcmp(operacion, "DELETE") == 0){
        //RESPUESTA AL CLIENTE si result 0:'0', result -2:'1', result -4:'2', result -3:'3', result -1:'4'
        //recibir user_name
        char user_name[MAX_USER_NAME_LENGTH] = {0};
        err = readLine(new_client_sock, (char *)user_name, sizeof(user_name));
        if (err <= 0){
            perror("Error al recibir user_name\n");
            close(new_client_sock);
            return NULL;
        }

        //recibir file_name
        char file_name[MAX_FILE_NAME_LENGTH] = {0};
        err = readLine(new_client_sock, (char *)file_name, sizeof(file_name));
        if (err <= 0){
            perror("Error al recibir file_name\n");
            close(new_client_sock);
            return NULL;
        }

        enviar_log_rpc(user_name, operacion, file_name, fecha_hora); //servidor rpc de logging

        int result = clients_list_delete_file(lista_global_clientes, user_name, file_name);
        //mostrar que se ha hecho la operacion
        printf("OPERATION DELETE FROM %s\n", user_name);

        // determinar y enviar codigo de resultado
        char respuesta;
        if (result == 0) {
            respuesta = '0'; //exito
        } else if (result == -2) {
            respuesta = '1'; // usuario no existe en la lista
        } else if (result == -4) {
            respuesta = '2'; // user_name no conectado
        } else if (result == -3){
            respuesta = '3'; //fichero no existe en la lista del usuario
        } else {
            respuesta = '4'; //error
        }

        //enviar respuesta
        err = sendMessage(new_client_sock, (char *)&respuesta, sizeof(char));
        if (err < 0){
            perror("Error al enviar respuesta\n");
            close(new_client_sock);
            return NULL;
        }

    } else if(strcmp(operacion, "LIST_USERS") == 0){
        //RESPUESTA AL CLIENTE si result >=0:'0', result -3:'1', result -2:'2', result -1:'3'
        //recibir user_name
        char user_name[MAX_USER_NAME_LENGTH] = {0};
        err = readLine(new_client_sock, (char *)user_name, sizeof(user_name));
        if (err <= 0){
            perror("Error al recibir user_name\n");
            close(new_client_sock);
            return NULL;
        }

        enviar_log_rpc(user_name, operacion, NULL, fecha_hora); //servidor rpc de logging

        pthread_mutex_lock(&lista_global_clientes->mutex);
        int result = clients_list_count_users(lista_global_clientes, user_name);
        //solo se hace el mutex para que la lista no se vea alterada entre contar y enviar aunque igualmente puede occ


        //mostrar que se ha hecho la operacion
        printf("OPERATION LIST_USERS FROM %s\n", user_name);

        // determinar y enviar codigo de resultado
        char respuesta;
        if (result >= 0) {
            respuesta = '0';
        } else if (result == -3) {
            respuesta = '1'; // usuario no existe en la lista
        } else if (result == -2) {
            respuesta = '2'; // user_name no conectado
        } else {
            respuesta = '3'; // error
        }

        //enviar respuesta
        err = sendMessage(new_client_sock, (char *)&respuesta, sizeof(char));
        if (err < 0){
            perror("Error al enviar respuesta\n");
            pthread_mutex_unlock(&lista_global_clientes->mutex);
            close(new_client_sock);
            return NULL;
        }

        if (result < 0){
            //o no existe el user o no esta conectado o error no hay que enviar nada mas
            pthread_mutex_unlock(&lista_global_clientes->mutex);
            close(new_client_sock);
            return NULL;
        }

        // Enviar número de usuarios conectados como cadena
        char total_str[16];
        snprintf(total_str, sizeof(total_str), "%d", result);
        sendMessage(new_client_sock, total_str, strlen(total_str) + 1);

        // Enviar la info de cada usuario conectado
        ClientNode * curr = lista_global_clientes->head;
        while (curr) {
            if (curr->status == 1) {
                sendMessage(new_client_sock, curr->user_name, strlen(curr->user_name) + 1);
                struct in_addr ip_addr;
                ip_addr.s_addr = htonl(curr->IP);
                sendMessage(new_client_sock, inet_ntoa(ip_addr), strlen(inet_ntoa(ip_addr)) + 1);
                char port_str[16];
                snprintf(port_str, sizeof(port_str), "%u", curr->port);
                sendMessage(new_client_sock, port_str, strlen(port_str) + 1);
            }
            curr = curr->next;
        }
        pthread_mutex_unlock(&lista_global_clientes->mutex);
    } else if (strcmp(operacion, "LIST_CONTENT") == 0){
        char user_name[MAX_USER_NAME_LENGTH] = {0};
        err = readLine(new_client_sock, user_name, sizeof(user_name));
        if (err <= 0) {
            perror("Error al recibir user_name\n");
            close(new_client_sock);
            return NULL;
        }

        enviar_log_rpc(user_name, operacion, NULL, fecha_hora); //servidor rpc de logging
    
        char target_user_name[MAX_USER_NAME_LENGTH] = {0};
        err = readLine(new_client_sock, target_user_name, sizeof(target_user_name));
        if (err <= 0) {
            perror("Error al recibir target_user_name\n");
            close(new_client_sock);
            return NULL;
        }
    
        // obtener una copia de los nombres de los ficheros del usuario objetivo
        char *file_names = NULL;
        int result = clients_list_content(lista_global_clientes, user_name, target_user_name, &file_names);
    
        // determinar y enviar codigo de resultado
        char respuesta;
        if (result >= 0) {
            respuesta = '0';
        } else if (result == -3) {
            respuesta = '3'; // target_user_name no existe
        } else if (result == -2) {
            respuesta = '2'; // user_name no conectado
        } else if (result == -4) {
            respuesta = '1'; // user_name no existe
        } else {
            respuesta = '4'; // error
        }
    
        err = sendMessage(new_client_sock, &respuesta, sizeof(char));
        if (err < 0 || respuesta != '0') {
            free(file_names);
            close(new_client_sock);
            return NULL;
        }

        // si todo fue bien, enviar numero de ficheros
        char buffer[16];
        snprintf(buffer, sizeof(buffer), "%d", result); // result contiene el numero de ficheros
        err = sendMessage(new_client_sock, buffer, strlen(buffer) + 1);
        if (err < 0) {
            free(file_names);
            close(new_client_sock);
            return NULL;
        }

        // enviar nombres de ficheros desde la copia, sin acceder a la lista compartida
        for (int i = 0; i < result; i++) {
            char *name = file_names + (size_t)i * MAX_FILE_NAME_LENGTH;
            sendMessage(new_client_sock, name, strlen(name) + 1);
        }
        free(file_names);
    
        printf("OPERATION LIST_CONTENT FROM %s\n", user_name);
    }else if (strcmp(operacion, "DISCONNECT") == 0){
        char user_name[MAX_USER_NAME_LENGTH] = {0};
        err = readLine(new_client_sock, user_name, sizeof(user_name));
        if (err <= 0) {
            perror("Error al recibir user_name\n");
            close(new_client_sock);
            return NULL;
        }

        enviar_log_rpc(user_name, operacion, NULL, fecha_hora); //servidor rpc de logging

        int result = clients_list_disconnect(lista_global_clientes, user_name);
        printf("OPERATION DISCONNECT FROM %s\n", user_name);

        // determinar y enviar codigo de resultado
        char respuesta;
        if (result == 0) {
            respuesta = '0'; //exito
        } else if (result == -3) {
            respuesta = '1'; // usuario no existe en la lista
        } else if (result == -2) {
            respuesta = '2'; // user_name no conectado
        } else {
            respuesta = '3'; // error
        }
    
        err = sendMessage(new_client_sock, &respuesta, sizeof(char));
        if (err < 0) {
            close(new_client_sock);
            return NULL;
        }
    }

    printf("s> ");
    fflush(stdout); //necesario para que se imprima inmediatamente
    close(new_client_sock);
    pthread_exit(NULL);
}
    

int main(int argc, char* argv[]){
    signal(SIGINT, handle_sigint); //handle ctrl+c
    //comprobaciones iniciales
    if (argc < 3){
        perror("Falta puerto o flag -p, formato es: ./servidor -p <PUERTO>\n");
        return -1;
    }
    
    if (strcmp(argv[1], "-p") != 0){
        perror("Flag incorrecto, el flag es '-p'\n");
        return -1;
    }

    if (strlen(argv[2]) > 5){
        perror("Puerto incorrecto\n");
        return -1;
    }

    //conversion de la entrada
    char *endptr;
    errno = 0;

    long port = strtol(argv[2], &endptr, 10);

    //verifica si no se pudo convertir en un numero o si hay caracteres sobrantes
    if (argv[2] == endptr || *endptr != '\0') {
        printf("Error: la cadena <PUERTO> no contiene un número válido\n");
        return -1;
    }

    //verifica el rango de puertos TCP (0 deja que el sistema operativo asigne uno libre)
    if (port < 0 || port > 65535) {
        printf("Error: <PUERTO> valor fuera de rango (0-65535)\n");
        return -1;
    }

    //inicializar ClientsList
    lista_global_clientes = clients_list_init();
    if (!lista_global_clientes)
        return -1;

    int *new_client_sock;
    struct sockaddr_in serv_addr, client_addr;
    socklen_t client_addr_size = sizeof(client_addr); //otra alternativa es que sea un int y hacer el casting en el accept
    socklen_t server_addr_size = sizeof(serv_addr);
    //crear el socket
    serv_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (serv_sock < 0){
        perror("Error al crear el socket");
        return -2;
    }

    //permite reiniciar el servidor en el mismo puerto sin esperar a que expire TIME_WAIT
    int reuse = 1;
    if (setsockopt(serv_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0){
        perror("Error en setsockopt(SO_REUSEADDR)");
        return -2;
    }

    //configuracion de direccion del servidor
    bzero((char *)&serv_addr, sizeof(serv_addr)); //limpiar estructura
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

    if(bind(serv_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0){
        perror("Error al enlazar");
        return -2;
    }

    // obtener la direccion y el puerto asignados
    if (getsockname(serv_sock, (struct sockaddr*)&serv_addr, &server_addr_size) < 0) {
        perror("Error en getsockname");
        exit(EXIT_FAILURE);
    }

    if (listen(serv_sock, SOMAXCONN) < 0){
        perror("Error en el listen");
        return -2;
    }
    //print para saber la ip y puerto concedidos al servidor por el SO
    printf("s> init server %s:%d\n", inet_ntoa(serv_addr.sin_addr), ntohs(serv_addr.sin_port));

    printf("s> ");
    fflush(stdout);

    int thread_ret_val = 0; //valor para el retorno de las funciones de threads para manejo de errores

    pthread_attr_t thread_attr;
    thread_ret_val = pthread_attr_init(&thread_attr);
    if (thread_ret_val != 0){
        perror("Error al inicializar la estructura de atributos de thread\n");
        return -1;
    }
    thread_ret_val = pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
    if (thread_ret_val != 0){
        perror("Error al modificar el atributo de thread a DETACHED\n");
        return -1;
    }

    while(1){
        new_client_sock = malloc(sizeof(int));
        *new_client_sock = accept(serv_sock, (struct sockaddr *)&client_addr, &client_addr_size);
        if (*new_client_sock < 0){
            perror("Error en el accept\n");
            free(new_client_sock);
            continue;
        }


        //crear hilo para atender_peticion
        pthread_t thread;
        thread_ret_val = pthread_create(&thread, &thread_attr, atender_peticion, (void *)new_client_sock);
        if (thread_ret_val != 0){
            perror("Error al crear el hilo para manejar peticion\n");
        }
    }
    pthread_attr_destroy(&thread_attr);
    close(serv_sock);

    return 0;
}